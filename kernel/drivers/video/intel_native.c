/* intel_native.c -- native modesetting on Gen9 laptops. See intel_native.h.
 *
 * What this file decides and what it delegates: the VBT says what is wired
 * where (intel_vbt.c); the panel says what it can do over AUX (DPCD, EDID);
 * intel_config.c turns a wanted mode into link, PLL, scaler, DDB and
 * watermark values; intel_hw.c programs them. This file owns the state in
 * between -- which output is lit with which configuration, and which
 * framebuffers exist in the GGTT -- and the policy: inherit the firmware's
 * picture when it is right, modeset when it is not, and put the previous mode
 * back when a new one fails. */
#include "drivers/video/intel_native.h"
#include "drivers/video/intel_hw.h"
#include "drivers/video/intel_vbt.h"
#include "drivers/video/intel_config.h"
#include "drivers/video/intel_dpaux.h"
#include "drivers/video/intel_mmio.h"
#include "drivers/video/framebuffer.h"
#include "display/edid.h"
#include "display/edid_modes.h"
#include "drivers/bus/pci.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "../../../user/lib/embk_monitor.h"

#define MAX_FBS     8u
#define GGTT_ALIGN  0x40000u        /* linear surfaces: 256 KiB (i915 intel_surf_alignment) */
#define PAGE        4096u

struct fbslot {
    int       used;
    uint32_t  ggtt, bytes, npages, w, h, pitch;
    uint64_t *pages;
};

struct output {
    uint8_t  kind, ddi, aux_ch;
    uint8_t  dpcd[16], edp_rev;
    struct intel_dp_caps caps;
    uint8_t  edid[256];
    unsigned edid_len;
    struct display_identity identity;
    int      has_identity;
    struct display_mode native;
    unsigned panel_bpp;
    uint8_t  iboost;
    struct intel_crtc_cfg cfg;       /* what is lit, when lit */
    int      lit, cfg_complete;      /* cfg_complete: built here, not read back from the firmware */
};

static struct {
    int ready;
    struct intel_regs regs;
    struct intel_hw hw;
    struct intel_vbt vbt;
    uint64_t aperture, aperture_size, gtt_phys;
    uint16_t lat[INTEL_WM_LEVELS];
    uint8_t  a_lanes;
    struct output edp;
    uint32_t fw_ggtt, fw_bytes;       /* the firmware framebuffer's GGTT range: never reused */
    struct fbslot fb[MAX_FBS];
    int boot_slot;                    /* the slot a boot-time modeset scans out, or -1 */
} g = { .boot_slot = -1 };

/* ---- physical memory windows ---------------------------------------------- */
static volatile uint8_t *map_phys(uint64_t phys, uint32_t len)
{
    uint64_t base = phys & ~(uint64_t)(PAGE - 1), span = (phys - base) + len;
    uint64_t v = vmm_map_mmio(base, span);
    return v ? (volatile uint8_t *)(uintptr_t)(v + (phys - base)) : 0;
}

static void unmap_phys(volatile uint8_t *virt, uint32_t len)
{
    uint64_t v = (uint64_t)(uintptr_t)virt, base = v & ~(uint64_t)(PAGE - 1);
    vmm_unmap_mmio(base, (v - base) + len);
}

/* ---- the VBT ---------------------------------------------------------------- */
static void load_vbt(const struct intel_native_probe *p)
{
    uint32_t asls = pci_read32(p->bus, p->dev, p->fn, 0xfc);
    if (!asls || asls == 0xffffffffu) { kprintf("intel-native: no OpRegion; using default port wiring\n"); return; }
    volatile uint8_t *op = map_phys(asls, INTEL_OPREGION_SIZE);
    if (!op) return;
    uint32_t off = 0, len = 0;
    uint64_t rvda = 0;
    int where = intel_vbt_locate(op, asls, &off, &rvda, &len);
    uint8_t *copy = (where && len && len <= 0x10000) ? (uint8_t *)kmalloc(len) : 0;
    if (copy && where == 1) {
        for (uint32_t i = 0; i < len; i++) copy[i] = op[off + i];
    } else if (copy && where == 2) {
        volatile uint8_t *v = map_phys(rvda, len);
        if (v) { for (uint32_t i = 0; i < len; i++) copy[i] = v[i]; unmap_phys(v, len); }
        else { kfree(copy); copy = 0; }
    }
    unmap_phys(op, INTEL_OPREGION_SIZE);
    if (!copy) { kprintf("intel-native: the OpRegion holds no VBT; using default port wiring\n"); return; }
    if (intel_vbt_parse(copy, len, p->pch, &g.vbt))
        kprintf("intel-native: the VBT is corrupt (checksum or layout); using default port wiring\n");
    kfree(copy);
}

/* ---- the panel ------------------------------------------------------------- */
static int rate_ok_dpll0(uint32_t rate, void *ctx)
{
    (void)ctx;
    if (g.hw.vco_khz == 8640000) return rate == 216000 || rate == 432000;
    return rate == 162000 || rate == 270000 || rate == 324000 || rate == 540000;
}

static void edp_source(struct intel_dp_source *s)
{
    *s = (struct intel_dp_source){ 540000, g.a_lanes, rate_ok_dpll0, 0 };
}

static int probe_edp(void)
{
    struct output *o = &g.edp;
    const struct intel_vbt_child *ch = 0;
    for (unsigned i = 0; i < g.vbt.child_count; i++)
        if (g.vbt.children[i].kind == INTEL_OUT_EDP) { ch = &g.vbt.children[i]; break; }
    o->kind = INTEL_OUT_EDP;
    o->ddi = ch ? ch->ddi : 0;
    o->aux_ch = ch && ch->aux_ch != INTEL_NO_AUX ? ch->aux_ch : 0;
    o->iboost = ch && ch->iboost ? ch->dp_iboost_level : 0;
    if (o->ddi != 0) {
        kprintf("intel-native: the VBT puts eDP on DDI %c; Gen9 drives eDP from DDI A only\n", 'A' + o->ddi);
        return -EMBK_ENOTSUP;
    }
    if (intel_dpcd_read(&g.regs, o->aux_ch, 0, o->dpcd, 16)) {
        kprintf("intel-native: the panel did not answer on AUX %c\n", 'A' + o->aux_ch);
        return -EMBK_EIO;
    }
    (void)intel_dpcd_read(&g.regs, o->aux_ch, 0x700, &o->edp_rev, 1);
    uint8_t rates[16];
    int table = o->edp_rev >= 0x03 && !intel_dpcd_read(&g.regs, o->aux_ch, 0x010, rates, 16);
    if (intel_dp_caps_parse(o->dpcd, table ? rates : 0, &o->caps)) {
        kprintf("intel-native: the panel's DPCD makes no sense (revision %02x)\n", o->dpcd[0]);
        return -EMBK_EIO;
    }
    if (!intel_aux_i2c_read(&g.regs, o->aux_ch, 0x50, 0, o->edid, 128)) {
        o->edid_len = 128;
        if (o->edid[126] && !intel_aux_i2c_read(&g.regs, o->aux_ch, 0x50, 128, o->edid + 128, 128))
            o->edid_len = 256;
        o->has_identity = !display_edid_parse(o->edid, 128, &o->identity);
    }
    struct display_mode modes[24];
    int n = o->edid_len ? edid_modes(o->edid, o->edid_len, modes, 24) : -1;
    for (int i = 0; i < n; i++)
        if (modes[i].flags & DISPLAY_MODE_PREFERRED) { o->native = modes[i]; break; }
    if (!o->native.clock_khz && n > 0) o->native = modes[0];
    if (!o->native.clock_khz && g.vbt.lfp_mode.clock_khz) o->native = g.vbt.lfp_mode;
    /* The panel's own EDID 1.4 bit depth first; the VBT's only for panels
     * whose EDID gives none (i915 intel_dp_max_bpp's order). */
    o->panel_bpp = 0;
    if (o->edid_len && o->edid[18] == 1 && o->edid[19] >= 4 && (o->edid[20] & 0x80)) {
        static const uint8_t bpc[8] = { 0, 6, 8, 10, 12, 14, 16, 0 };
        unsigned b = bpc[(o->edid[20] >> 4) & 7];
        if (b) o->panel_bpp = b >= 8 ? 24 : 18;
    }
    if (!o->panel_bpp) o->panel_bpp = g.vbt.edp_bpp && g.vbt.edp_bpp < 24 ? g.vbt.edp_bpp : 24;
    return 0;
}

/* The firmware's eDP pipe, read back so it can be turned off cleanly. */
static void read_firmware_edp(void)
{
    const struct intel_regs *r = &g.regs;
    uint32_t conf = intel_rd(r, 0x7f008), fn = intel_rd(r, 0x6f400);
    if (!(conf >> 31) || !(fn >> 31)) return;
    struct intel_crtc_cfg *c = &g.edp.cfg;
    memset(c, 0, sizeof *c);
    unsigned in = (fn >> 12) & 7u;
    c->pipe = in == 5 ? 1 : in == 6 ? 2 : 0;
    c->transcoder = INTEL_TRANS_EDP;
    c->ddi = 0;
    c->kind = INTEL_OUT_EDP;
    c->aux_ch = g.edp.aux_ch;
    c->dpll = (uint8_t)((intel_rd(r, 0x6c05c) >> 1) & 3u);
    static const uint32_t rate_of[6] = { 540000, 270000, 162000, 324000, 216000, 432000 };
    unsigned code = (intel_rd(r, 0x6c058) >> (c->dpll * 6 + 1)) & 7u;
    c->link.rate = code < 6 ? rate_of[code] : 0;
    c->link.lanes = (uint8_t)(((fn >> 1) & 7u) + 1u);
    uint32_t b = 0x6f000;
    c->timing.hdisplay = (uint16_t)((intel_rd(r, b) & 0x1fff) + 1);
    c->timing.htotal = (uint16_t)((intel_rd(r, b) >> 16) + 1);
    c->timing.hsync_start = (uint16_t)((intel_rd(r, b + 8) & 0x1fff) + 1);
    c->timing.hsync_end = (uint16_t)((intel_rd(r, b + 8) >> 16) + 1);
    c->timing.vdisplay = (uint16_t)((intel_rd(r, b + 0xc) & 0x1fff) + 1);
    c->timing.vtotal = (uint16_t)((intel_rd(r, b + 0xc) >> 16) + 1);
    c->timing.vsync_start = (uint16_t)((intel_rd(r, b + 0x14) & 0x1fff) + 1);
    c->timing.vsync_end = (uint16_t)((intel_rd(r, b + 0x14) >> 16) + 1);
    uint32_t lm = intel_rd(r, b + 0x40) & 0xffffff, ln = intel_rd(r, b + 0x44) & 0xffffff;
    c->timing.clock_khz = ln ? (uint32_t)((uint64_t)lm * c->link.rate / ln) : 0;
    if (fn & (1u << 16)) c->timing.flags |= DISPLAY_MODE_HSYNC_POSITIVE;
    if (fn & (1u << 17)) c->timing.flags |= DISPLAY_MODE_VSYNC_POSITIVE;
    uint32_t src = intel_rd(r, 0x6001c + c->pipe * 0x1000u);
    c->src_w = (uint16_t)((src >> 16) + 1);
    c->src_h = (uint16_t)((src & 0xffff) + 1);
    c->bpc = ((fn >> 20) & 7u) == 2 ? 6 : 8;
    c->pfit_enabled = (intel_rd(r, 0x68180 + c->pipe * 0x800u) >> 31) & 1;
    c->surf_ggtt = intel_rd(r, 0x7019c + c->pipe * 0x1000u) & ~0xfffu;
    c->stride = (intel_rd(r, 0x70188 + c->pipe * 0x1000u) & 0x7ffu) * 64u;
    g.edp.lit = 1;
    g.edp.cfg_complete = 0;
    g.hw.dpll_users[c->dpll]++;
    g.fw_ggtt = c->surf_ggtt;
    g.fw_bytes = c->stride * c->src_h;
}

/* ---- framebuffers in the GGTT --------------------------------------------- */
static int ggtt_overlaps(uint32_t start, uint32_t bytes)
{
    uint32_t end = start + bytes;
    if (g.fw_bytes && start < g.fw_ggtt + g.fw_bytes && g.fw_ggtt < end) return 1;
    for (unsigned i = 0; i < MAX_FBS; i++)
        if (g.fb[i].used && start < g.fb[i].ggtt + g.fb[i].bytes && g.fb[i].ggtt < end) return 1;
    return 0;
}

static void write_ptes(uint32_t ggtt, const uint64_t *pages, uint32_t n)
{
    volatile uint8_t *gsm = map_phys(g.gtt_phys + (uint64_t)(ggtt / PAGE) * 8u, n * 8u);
    if (!gsm) return;
    intel_ggtt_map(&g.regs, (volatile uint64_t *)gsm, pages, n);
    unmap_phys(gsm, n * 8u);
}

static int fb_alloc(uint32_t w, uint32_t h, uint32_t pitch)
{
    unsigned s = 0;
    while (s < MAX_FBS && g.fb[s].used) s++;
    if (s == MAX_FBS) return -1;
    uint32_t bytes = (pitch * h + PAGE - 1) & ~(PAGE - 1), npages = bytes / PAGE;
    uint32_t span = (bytes + GGTT_ALIGN - 1) & ~(GGTT_ALIGN - 1);
    if (span > g.aperture_size) return -1;
    uint32_t at = (uint32_t)((g.aperture_size - span) & ~(uint64_t)(GGTT_ALIGN - 1));
    for (;;) {                                            /* top down, first fit */
        if (!ggtt_overlaps(at, span)) break;
        if (at < GGTT_ALIGN) return -1;
        at -= GGTT_ALIGN;
    }
    uint64_t *pages = (uint64_t *)kmalloc(npages * sizeof *pages);
    if (!pages) return -1;
    for (uint32_t i = 0; i < npages; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            while (i--) pmm_free_page(pages[i]);
            kfree(pages);
            return -1;
        }
    }
    write_ptes(at, pages, npages);
    /* Clear it through the aperture -- the path the display reads -- so the
     * first frame shows black, not whatever the pages held. */
    volatile uint8_t *ap = (volatile uint8_t *)(uintptr_t)vmm_map_mmio_wc(g.aperture + at, bytes);
    if (ap) { memset((void *)(uintptr_t)ap, 0, bytes); vmm_unmap_mmio((uint64_t)(uintptr_t)ap, bytes); }
    g.fb[s] = (struct fbslot){ 1, at, span, npages, w, h, pitch, pages };
    return (int)s;
}

static void fb_free(unsigned s)
{
    struct fbslot *f = &g.fb[s];
    if (!f->used) return;
    volatile uint8_t *gsm = map_phys(g.gtt_phys + (uint64_t)(f->ggtt / PAGE) * 8u, f->npages * 8u);
    if (gsm) {
        volatile uint64_t *pte = (volatile uint64_t *)gsm;
        for (uint32_t i = 0; i < f->npages; i++) pte[i] = 0;
        intel_wr(&g.regs, 0x101008, 1u);                  /* GFX_FLSH_CNTL */
        unmap_phys(gsm, f->npages * 8u);
    }
    for (uint32_t i = 0; i < f->npages; i++) pmm_free_page(f->pages[i]);
    kfree(f->pages);
    *f = (struct fbslot){ 0, 0, 0, 0, 0, 0, 0, 0 };
}

/* ---- configurations --------------------------------------------------------- */
static int build_cfg(uint32_t w, uint32_t h, uint32_t surf, uint32_t pitch, struct intel_crtc_cfg *c)
{
    struct output *o = &g.edp;
    const struct display_mode *n = &o->native;
    memset(c, 0, sizeof *c);
    c->pipe = INTEL_PIPE_A;
    c->transcoder = INTEL_TRANS_EDP;
    c->ddi = 0;
    c->kind = INTEL_OUT_EDP;
    c->aux_ch = o->aux_ch;
    c->dpll = 0;
    c->timing = *n;
    c->src_w = (uint16_t)w;
    c->src_h = (uint16_t)h;
    if (w != n->hdisplay || h != n->vdisplay) {
        if (intel_pfit_compute(w, h, n->hdisplay, n->vdisplay, &c->pfit)) return -EMBK_EINVAL;
        c->pfit_enabled = 1;
    }
    struct intel_dp_source src;
    edp_source(&src);
    static const uint32_t vbt_rate[3] = { 162000, 270000, 540000 };
    int vr = g.vbt.edp_fast_link.valid ? (int)vbt_rate[g.vbt.edp_fast_link.rate_code] : 0;
    unsigned vl = g.vbt.edp_fast_link.valid ? g.vbt.edp_fast_link.lanes : 0;
    if (intel_dp_pick_link(&o->caps, &src, n, 1, o->panel_bpp, vr, vl, &c->link)) return -EMBK_ERANGE;
    c->bpc = c->link.bpp == 18 ? 6 : 8;
    c->enhanced_framing = o->caps.enhanced_framing;
    c->downspread = o->caps.downspread;
    c->tps3 = o->caps.tps3;
    c->training_interval = o->caps.training_interval;
    c->dpcd_rev = o->dpcd[0];
    c->keep_firmware_trans = 1;
    c->iboost = o->iboost;
    c->surf_ggtt = surf;
    c->stride = pitch;
    c->rgb_order = 0;
    unsigned widths[1] = { w };
    intel_ddb_split(1, widths, &c->plane_ddb, &c->cursor_ddb);
    if (intel_plane_wm(g.lat, n->clock_khz, n->htotal, w, 4, (uint32_t)(c->plane_ddb.end - c->plane_ddb.start), c->wm) < 0)
        return -EMBK_ENOSPC;
    return 0;
}

/* The same picture on the same timing: only the surface may differ. Clocks
 * within 0.5 %, because one read back from the firmware's M/N is rounded. */
static int same_scanout(const struct intel_crtc_cfg *a, const struct intel_crtc_cfg *b)
{
    uint32_t d = a->timing.clock_khz > b->timing.clock_khz ? a->timing.clock_khz - b->timing.clock_khz
                                                           : b->timing.clock_khz - a->timing.clock_khz;
    return d * 200u <= b->timing.clock_khz && a->timing.hdisplay == b->timing.hdisplay &&
           a->timing.vdisplay == b->timing.vdisplay && a->timing.htotal == b->timing.htotal &&
           a->timing.vtotal == b->timing.vtotal && a->src_w == b->src_w && a->src_h == b->src_h &&
           a->pfit_enabled == b->pfit_enabled && a->bpc == b->bpc && a->pipe == b->pipe &&
           (!a->pfit_enabled || (a->pfit.x == b->pfit.x && a->pfit.y == b->pfit.y &&
                                 a->pfit.w == b->pfit.w && a->pfit.h == b->pfit.h));
}

/* Put `next` on the panel. A plane flip when only the surface changes; a
 * full disable and enable otherwise, with the previous mode put back if the
 * new one fails. */
static int show(struct intel_crtc_cfg *next)
{
    struct output *o = &g.edp;
    if (o->lit && same_scanout(&o->cfg, next)) {
        struct intel_crtc_cfg flip = o->cfg;            /* the pipe as it runs, new surface */
        flip.surf_ggtt = next->surf_ggtt;
        flip.stride = next->stride;
        intel_hw_plane_flip(&g.hw, &flip);
        o->cfg = flip;
        return 0;
    }
    struct intel_crtc_cfg prev = o->cfg;
    int was_lit = o->lit;
    if (was_lit) intel_hw_disable(&g.hw, &prev, 1);
    o->lit = 0;
    if (next->timing.clock_khz > g.hw.cdclk_khz) {
        uint32_t cd = intel_cdclk_pick(next->timing.clock_khz, g.hw.vco_khz);
        if (!cd || intel_hw_set_cdclk(&g.hw, cd)) kprintf("intel-native: CDCLK could not rise: %s\n", g.hw.last_error);
    }
    struct intel_dp_source src;
    edp_source(&src);
    int rc = intel_hw_enable(&g.hw, next, &o->caps, &src);
    if (!rc) {
        o->cfg = *next;
        o->lit = 1;
        o->cfg_complete = 1;
        for (unsigned i = 0; i < 3; i++) intel_hw_wait_vblank(&g.hw, next->pipe);   /* three frames, then look */
        if (intel_hw_underrun(&g.hw, next->pipe))
            kprintf("intel-native: pipe %c underran after the modeset -- watermarks or DDB too small\n", 'A' + next->pipe);
        kprintf("intel-native: eDP %ux%u on a %ux%u panel, %u.%02u Gbps x%u, %u bpc%s\n",
                next->src_w, next->src_h, next->timing.hdisplay, next->timing.vdisplay,
                next->link.rate / 100000, (next->link.rate / 1000) % 100, next->link.lanes, next->bpc,
                next->pfit_enabled ? ", panel fitter" : "");
        return 0;
    }
    kprintf("intel-native: %ux%u failed: %s\n", next->src_w, next->src_h, g.hw.last_error);
    if (was_lit) {
        struct intel_crtc_cfg again;
        if (!build_cfg(prev.src_w, prev.src_h, prev.surf_ggtt, prev.stride, &again) &&
            !intel_hw_enable(&g.hw, &again, &o->caps, &src)) {
            o->cfg = again;
            o->lit = 1;
            o->cfg_complete = 1;
            kprintf("intel-native: the previous mode is back\n");
        } else {
            kprintf("intel-native: the previous mode could not be restored either: %s\n", g.hw.last_error);
        }
    }
    return rc;
}

/* ---- init and the gpu_driver hooks ----------------------------------------- */
int intel_native_init(const struct intel_native_probe *p)
{
    memset(&g, 0, sizeof g);
    g.boot_slot = -1;
    intel_regs_init_mmio(&g.regs, p->mmio);
    g.aperture = p->aperture;
    g.aperture_size = p->aperture_size;
    g.gtt_phys = p->bar0 + p->bar0_size / 2;
    if (p->bar0_size < 0x1000000u || !p->aperture_size) {
        kprintf("intel-native: BAR0 %lu MiB / aperture %lu MiB: no GGTT window to place framebuffers\n",
                (unsigned long)(p->bar0_size >> 20), (unsigned long)(p->aperture_size >> 20));
        return -EMBK_ENODEV;
    }
    load_vbt(p);
    intel_hw_init(&g.hw, &g.regs, p->pch, g.vbt.valid ? &g.vbt : 0);
    uint32_t w0 = 0, w1 = 1;
    if (intel_hw_pcode_read(&g.hw, INTEL_PCODE_READ_MEM_LATENCY, &w0) ||
        intel_hw_pcode_read(&g.hw, INTEL_PCODE_READ_MEM_LATENCY, &w1)) {
        kprintf("intel-native: pcode would not give the memory latencies; no watermarks, no modesetting\n");
        return -EMBK_EIO;
    }
    intel_wm_latency_adjust(w0, w1, g.lat);
    g.a_lanes = (intel_rd(&g.regs, 0x64000) & (1u << 4)) ? 4 : 2;
    if (g.a_lanes == 2 && !intel_vbt_child_for_ddi(&g.vbt, 4)) g.a_lanes = 4;   /* no DDI E: A has all four */
    int rc = probe_edp();
    if (rc) return rc;
    read_firmware_edp();
    if (!g.edp.native.clock_khz && g.edp.lit) g.edp.native = g.edp.cfg.timing;
    if (!g.edp.native.clock_khz || !display_mode_sane(&g.edp.native)) {
        kprintf("intel-native: no usable native timing for the panel\n");
        return -EMBK_ENODEV;
    }
    g.ready = 1;
    kprintf("intel-native: %04x, VBT %s (BDB %u, %u outputs), panel %s %ux%u %u.%03u MHz, DPCD %u.%u%s, "
            "CDCLK %u kHz, memory latencies %u/%u/%u/%u/%u/%u/%u/%u us\n",
            p->device_id, g.vbt.valid ? "ok" : "absent", g.vbt.bdb_version, g.vbt.child_count,
            g.edp.has_identity ? g.edp.identity.name : "(no EDID)", g.edp.native.hdisplay, g.edp.native.vdisplay,
            g.edp.native.clock_khz / 1000, g.edp.native.clock_khz % 1000, g.edp.dpcd[0] >> 4, g.edp.dpcd[0] & 0xf,
            g.edp.caps.rate_table ? " (rate table)" : "", g.hw.cdclk_khz,
            g.lat[0], g.lat[1], g.lat[2], g.lat[3], g.lat[4], g.lat[5], g.lat[6], g.lat[7]);
    return 0;
}

uint32_t intel_native_pitch(uint32_t width)
{
    return (width * 4u + 63u) & ~63u;
}

int intel_native_boot_mode(struct gpu_mode *out)
{
    if (!g.ready) return 0;
    const struct display_mode *n = &g.edp.native;
    const struct intel_crtc_cfg *c = &g.edp.cfg;
    uint32_t diff = c->timing.clock_khz > n->clock_khz ? c->timing.clock_khz - n->clock_khz : n->clock_khz - c->timing.clock_khz;
    if (g.edp.lit && c->src_w == n->hdisplay && c->src_h == n->vdisplay && c->timing.hdisplay == n->hdisplay &&
        c->timing.vdisplay == n->vdisplay && diff * 200u <= n->clock_khz && !c->pfit_enabled) {
        kprintf("intel-native: the firmware's mode is the panel's own; keeping it\n");
        return 0;
    }
    kprintf("intel-native: the firmware left %ux%u%s; setting the panel's %ux%u\n", c->src_w, c->src_h,
            g.edp.lit ? "" : " (or nothing)", n->hdisplay, n->vdisplay);
    uint32_t pitch = intel_native_pitch(n->hdisplay);
    int s = fb_alloc(n->hdisplay, n->vdisplay, pitch);
    if (s < 0) { kprintf("intel-native: no memory for a framebuffer; keeping the firmware's mode\n"); return 0; }
    struct intel_crtc_cfg next;
    if (build_cfg(n->hdisplay, n->vdisplay, g.fb[s].ggtt, pitch, &next) || show(&next)) {
        fb_free((unsigned)s);
        return 0;
    }
    g.boot_slot = s;
    *out = (struct gpu_mode){ g.aperture + g.fb[s].ggtt, n->hdisplay, n->vdisplay, pitch, 32, FB_FORMAT_BGR };
    return 1;
}

uint32_t intel_native_resource_id(void)
{
    return g.boot_slot >= 0 ? (uint32_t)g.boot_slot + 1u : 0u;
}

bool intel_native_prepare(struct gpu_target *t, void *back, uint64_t bytes, uint32_t w, uint32_t h)
{
    (void)back;
    if (!g.ready) return false;
    uint32_t pitch = intel_native_pitch(w);
    if (bytes != (uint64_t)pitch * h || w > g.edp.native.hdisplay || h > g.edp.native.vdisplay) return false;
    struct intel_crtc_cfg probe;
    if (build_cfg(w, h, 0, pitch, &probe)) return false;     /* refuse what the panel fitter cannot do */
    int s = fb_alloc(w, h, pitch);
    if (s < 0) return false;
    t->resource_id = (uint32_t)s + 1u;
    t->mode = (struct gpu_mode){ g.aperture + g.fb[s].ggtt, w, h, pitch, 32, FB_FORMAT_BGR };
    return true;
}

bool intel_native_select(const struct gpu_target *t)
{
    if (!g.ready) return false;
    uint32_t surf;
    if (t->resource_id) {
        unsigned s = t->resource_id - 1u;
        if (s >= MAX_FBS || !g.fb[s].used) return false;
        surf = g.fb[s].ggtt;
    } else {
        surf = g.fw_ggtt;                                    /* the firmware's own framebuffer */
    }
    struct intel_crtc_cfg next;
    if (build_cfg(t->mode.width, t->mode.height, surf, t->mode.pitch, &next)) return false;
    return show(&next) == 0;
}

bool intel_native_release(const struct gpu_target *t)
{
    if (!t->resource_id) return true;                        /* firmware memory: nothing to give back */
    unsigned s = t->resource_id - 1u;
    if (s >= MAX_FBS || !g.fb[s].used) return true;
    if (g.edp.lit && g.edp.cfg.surf_ggtt == g.fb[s].ggtt) return false;   /* still on screen */
    if ((int)s == g.boot_slot) g.boot_slot = -1;
    fb_free(s);
    return true;
}

void intel_native_describe(struct embk_monitor_info *info)
{
    info->flags |= EMBK_MONITOR_CONNECTED;
    if (!g.ready) return;
    info->flags |= EMBK_MONITOR_MODESET;
    if (g.edp.has_identity) display_identity_describe(&g.edp.identity, info);
    const struct display_mode *n = &g.edp.native;
    /* What the panel fitter can show: nothing larger than the panel. */
    unsigned out = 0, have_native = 0;
    for (unsigned i = 0; i < info->mode_count; i++) {
        struct embk_monitor_mode m = info->modes[i];
        if (m.width > n->hdisplay || m.height > n->vdisplay) continue;
        if (m.width == n->hdisplay && m.height == n->vdisplay) {
            have_native = 1;
            m.flags |= EMBK_MONITOR_PREFERRED;
            m.refresh_millihz = display_mode_refresh_mhz(n);
        }
        info->modes[out++] = m;
    }
    if (!have_native && out < EMBK_MONITOR_MAX_MODES)
        info->modes[out++] = (struct embk_monitor_mode){ n->hdisplay, n->vdisplay, display_mode_refresh_mhz(n),
                                                         EMBK_MONITOR_PREFERRED };
    info->mode_count = out;
}
