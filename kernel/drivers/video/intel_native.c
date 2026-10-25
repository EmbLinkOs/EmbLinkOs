/* intel_native.c -- native modesetting on Gen9 laptops. See intel_native.h.
 *
 * What this file decides and what it delegates: the VBT says what is wired
 * where (intel_vbt.c); the panel and any monitor say what they can do (DPCD
 * over AUX, EDID over AUX or GMBUS); intel_config.c turns a wanted mode into
 * link, PLL, scaler, DDB and watermark values; intel_hw.c programs them. This
 * file owns the state in between -- which output is lit with which
 * configuration, and which framebuffers exist in the GGTT -- and the policy:
 * inherit the firmware's picture when it is right, modeset when it is not,
 * and put the previous picture back when a new one fails.
 *
 * Several outputs: the panel (output 0, pipe A) and the board's DP and HDMI
 * ports, from the VBT. A monitor is found by its port's hotplug line and then
 * asked what it is; the desktop is one surface, and each lit pipe scans its
 * own rectangle of it (display/layout.h). Gen9 has three pipes, so three
 * outputs at most are lit together. */
#include "drivers/video/intel_native.h"
#include "drivers/video/intel_hw.h"
#include "drivers/video/intel_vbt.h"
#include "drivers/video/intel_config.h"
#include "drivers/video/intel_dpaux.h"
#include "drivers/video/intel_gmbus.h"
#include "drivers/video/intel_mmio.h"
#include "drivers/video/intel_blt.h"
#include "drivers/video/intel_compose.h"
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
#define MAX_OUT     4u              /* the panel, and up to three ports: DDI B-D */
#define NPIPES      3u
#define DDI_MAX_RATE 540000u        /* Gen9 DDI B-D: HBR2 */
#define HDMI_MAX_KHZ 300000u        /* Gen9 HDMI 1.4 TMDS */
#define DVI_MAX_KHZ  165000u        /* single-link DVI */

struct fbslot {
    int       used;
    uint32_t  ggtt, bytes, npages, w, h, pitch;
    uint64_t *pages;
};

struct output {
    uint8_t  kind, ddi, aux_ch;      /* the port, as the VBT wires it */
    uint8_t  sink;                   /* what answered on it (INTEL_OUT_*); the panel's is EDP */
    uint8_t  gmbus_pin, hdmi_level, dual_mode, hdmi_iboost;
    uint32_t max_rate;               /* the port's DP link-rate ceiling, kHz */
    char     name[16];               /* "eDP-1", "HDMI-1", "DP-1" */
    uint8_t  dpcd[16], edp_rev;
    struct intel_dp_caps caps;
    uint8_t  edid[256];
    unsigned edid_len;
    struct display_identity identity;
    int      has_identity;
    struct display_mode native;
    unsigned panel_bpp;
    uint8_t  iboost;
    uint8_t  connected, hpd, retries;
    unsigned nmodes;                 /* an external monitor: what it can show here, preferred first */
    struct display_mode modes[GPU_OUTPUT_MODES];
    struct intel_crtc_cfg cfg;       /* what is lit, when lit */
    int      lit, cfg_complete;      /* cfg_complete: built here, not read back from the firmware */
};

static struct {
    int ready;
    uint8_t bus, dev, fn;
    struct intel_regs regs;
    struct intel_hw hw;
    struct intel_vbt vbt;
    uint64_t aperture, aperture_size, gtt_phys, gtt_window;   /* gtt_window: BAR0's upper half, the GSM's most */
    uint16_t lat[INTEL_WM_LEVELS];
    uint8_t  a_lanes;
    struct output out[MAX_OUT];       /* out[0]: the panel */
    unsigned nout;
    int      polled;
    uint32_t fw_ggtt, fw_bytes;       /* the firmware framebuffer's GGTT range: never reused */
    struct fbslot fb[MAX_FBS];
    int boot_slot;                    /* the slot a boot-time modeset scans out, or -1 */
} g = { .boot_slot = -1 };
#define EDP (&g.out[0])

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
    struct output *o = EDP;
    const struct intel_vbt_child *ch = 0;
    for (unsigned i = 0; i < g.vbt.child_count; i++)
        if (g.vbt.children[i].kind == INTEL_OUT_EDP) { ch = &g.vbt.children[i]; break; }
    o->kind = INTEL_OUT_EDP;
    o->sink = INTEL_OUT_EDP;
    o->connected = 1;
    memcpy(o->name, "eDP-1", 6);
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

/* ---- the ports ------------------------------------------------------------- */
/* The VBT's per-port DP rate ceiling has two encodings: BDB 216-229 count
 * down from HBR3, 230 on count up from "the default" (i915 intel_bios.c). */
static uint32_t vbt_rate_limit(unsigned code)
{
    static const uint32_t old_codes[4] = { 810000, 540000, 270000, 162000 };
    static const uint32_t new_codes[5] = { 0, 162000, 270000, 540000, 810000 };
    if (g.vbt.bdb_version >= 230) return code < 5 ? new_codes[code] : 0;
    return code < 4 ? old_codes[code] : 0;
}

/* The board's DP and HDMI ports, from the VBT. Without a VBT nothing says
 * which DDI goes to which connector, and a guess could drive a lane into
 * nothing: no external outputs then, only the panel. */
static void probe_ports(void)
{
    unsigned ndp = 0, nhdmi = 0, ndvi = 0;
    g.nout = 1;
    if (!g.vbt.valid) { kprintf("intel-native: no VBT, so no external ports: only the panel is driven\n"); return; }
    for (unsigned i = 0; i < g.vbt.child_count && g.nout < MAX_OUT; i++) {
        const struct intel_vbt_child *c = &g.vbt.children[i];
        if (c->kind != INTEL_OUT_DP && c->kind != INTEL_OUT_HDMI && c->kind != INTEL_OUT_DVI) continue;
        if (c->ddi < 1 || c->ddi > 3) {
            kprintf("intel-native: a port on DDI %c is not driven (DDI B-D only)\n", 'A' + c->ddi);
            continue;
        }
        struct output *o = &g.out[g.nout++];
        memset(o, 0, sizeof *o);
        o->kind = c->kind; o->ddi = c->ddi; o->aux_ch = c->aux_ch;
        o->gmbus_pin = c->gmbus_pin; o->hdmi_level = c->hdmi_level; o->dual_mode = c->dual_mode;
        o->iboost = c->iboost ? c->dp_iboost_level : 0;
        o->hdmi_iboost = c->iboost ? c->hdmi_iboost_level : 0;
        uint32_t limit = vbt_rate_limit(c->dp_max_link_rate);
        o->max_rate = limit && limit < DDI_MAX_RATE ? limit : DDI_MAX_RATE;
        if (c->kind == INTEL_OUT_DP) snprintf(o->name, sizeof o->name, "DP-%u", ++ndp);
        else if (c->kind == INTEL_OUT_HDMI) snprintf(o->name, sizeof o->name, "HDMI-%u", ++nhdmi);
        else snprintf(o->name, sizeof o->name, "DVI-%u", ++ndvi);
    }
}

static int rate_ok_ddi(uint32_t rate, void *ctx)
{
    (void)ctx;
    return intel_dpll_link_rate_code(rate) >= 0;            /* what DPLL1-3 can make */
}

static void ext_source(const struct output *o, struct intel_dp_source *s)
{
    *s = (struct intel_dp_source){ o->max_rate, 4, rate_ok_ddi, 0 };
}

/* Can this monitor be shown this mode from this port? */
static int mode_fits(const struct output *o, const struct display_mode *m)
{
    if (!display_mode_sane(m) || (m->flags & DISPLAY_MODE_INTERLACED)) return 0;
    if (m->hdisplay > 5120 || m->vdisplay > 4096) return 0;          /* a Gen9 plane's largest */
    if (!intel_cdclk_pick(m->clock_khz, g.hw.vco_khz)) return 0;      /* faster than any display clock */
    if (o->sink == INTEL_OUT_DP) {
        struct intel_dp_source src;
        struct intel_dp_link link;
        ext_source(o, &src);
        return !intel_dp_pick_link(&o->caps, &src, m, 0, 24, 0, 0, &link);
    }
    if (m->clock_khz > (o->sink == INTEL_OUT_DVI ? DVI_MAX_KHZ : HDMI_MAX_KHZ)) return 0;
    struct intel_wrpll w;
    return !intel_wrpll_compute(m->clock_khz, &w);
}

static uint32_t off_60hz(const struct display_mode *m)
{
    uint32_t r = display_mode_refresh_mhz(m);
    return r > 60000 ? r - 60000 : 60000 - r;
}

/* What a monitor can show here: its preferred mode first, when this port can
 * carry it; then one timing per size, largest first -- the one nearest 60 Hz,
 * which is what a monitor is surest to take. */
static void build_modes(struct output *o)
{
    struct display_mode all[32], keep[32];
    int n = edid_modes(o->edid, o->edid_len, all, 32);
    unsigned k = 0;
    memset(&o->native, 0, sizeof o->native);
    for (int i = 0; i < n; i++)
        if ((all[i].flags & DISPLAY_MODE_PREFERRED) && mode_fits(o, &all[i])) { o->native = all[i]; break; }
    for (int i = 0; i < n; i++) {
        const struct display_mode *m = &all[i];
        if (!mode_fits(o, m)) continue;
        if (o->native.clock_khz && m->hdisplay == o->native.hdisplay && m->vdisplay == o->native.vdisplay) continue;
        int at = -1;
        for (unsigned j = 0; j < k; j++) if (keep[j].hdisplay == m->hdisplay && keep[j].vdisplay == m->vdisplay) at = (int)j;
        if (at >= 0) { if (off_60hz(m) < off_60hz(&keep[at])) keep[at] = *m; continue; }
        keep[k++] = *m;
    }
    for (unsigned i = 1; i < k; i++)                             /* largest first */
        for (unsigned j = i; j > 0 && (uint32_t)keep[j].hdisplay * keep[j].vdisplay >
                                       (uint32_t)keep[j - 1].hdisplay * keep[j - 1].vdisplay; j--) {
            struct display_mode t = keep[j]; keep[j] = keep[j - 1]; keep[j - 1] = t;
        }
    o->nmodes = 0;
    if (o->native.clock_khz) o->modes[o->nmodes++] = o->native;
    for (unsigned i = 0; i < k && o->nmodes < GPU_OUTPUT_MODES; i++) o->modes[o->nmodes++] = keep[i];
    if (!o->native.clock_khz && o->nmodes) o->native = o->modes[0];
}

static void read_edid(struct output *o, int over_aux)
{
    o->edid_len = 0;
    int rc = over_aux ? intel_aux_i2c_read(&g.regs, o->aux_ch, 0x50, 0, o->edid, 128)
                      : (o->gmbus_pin ? intel_gmbus_read(&g.regs, o->gmbus_pin, 0x50, 0, o->edid, 128) : -1);
    if (rc) return;
    o->edid_len = 128;
    if (!o->edid[126]) return;
    rc = over_aux ? intel_aux_i2c_read(&g.regs, o->aux_ch, 0x50, 128, o->edid + 128, 128)
                  : intel_gmbus_read(&g.regs, o->gmbus_pin, 0x50, 128, o->edid + 128, 128);
    if (!rc) o->edid_len = 256;
}

/* Something is on the port's hotplug line: what is it? A DP port asks over
 * AUX; an HDMI port -- or a DP++ port whose AUX is silent, which is a passive
 * HDMI adaptor -- reads the EDID over DDC, and the EDID says HDMI or DVI.
 * 1 when it is a monitor this port can drive in at least one mode. */
static int detect(struct output *o)
{
    o->sink = 0; o->edid_len = 0; o->has_identity = 0; o->nmodes = 0;
    if (o->kind == INTEL_OUT_DP && o->aux_ch != INTEL_NO_AUX && !intel_hw_aux_power(&g.hw) &&
        !intel_dpcd_read(&g.regs, o->aux_ch, 0, o->dpcd, 16) && o->dpcd[0] &&
        !intel_dp_caps_parse(o->dpcd, 0, &o->caps)) {
        o->sink = INTEL_OUT_DP;
        read_edid(o, 1);
    } else if (o->kind != INTEL_OUT_DP || o->dual_mode) {
        read_edid(o, 0);
        if (o->edid_len)
            o->sink = o->kind == INTEL_OUT_DVI || !edid_hdmi(o->edid, o->edid_len) ? INTEL_OUT_DVI : INTEL_OUT_HDMI;
    }
    if (!o->sink || !o->edid_len) return 0;
    o->has_identity = !display_edid_parse(o->edid, 128, &o->identity);
    build_modes(o);
    return o->nmodes > 0;
}

/* Pipes the firmware lit for an external monitor (the boot picture mirrored
 * to it): turned off at init, so every pipe this driver uses starts from what
 * it knows. The monitor is found again by the first poll and joins the
 * desktop like any other. */
static void firmware_externals_off(void)
{
    const struct intel_regs *r = &g.regs;
    for (unsigned t = 0; t < NPIPES; t++) {
        uint32_t conf = intel_rd(r, 0x70008 + t * 0x1000u), fn = intel_rd(r, 0x60400 + t * 0x1000u);
        if (!(conf >> 31) || !(fn >> 31) || !((fn >> 28) & 7u)) continue;   /* DDI A is the panel's */
        struct intel_crtc_cfg c;
        memset(&c, 0, sizeof c);
        c.pipe = (uint8_t)t;
        c.transcoder = (uint8_t)t;
        c.ddi = (uint8_t)((fn >> 28) & 7u);
        unsigned mode = (fn >> 24) & 7u;
        c.kind = mode == 0 ? INTEL_OUT_HDMI : mode == 1 ? INTEL_OUT_DVI : INTEL_OUT_DP;
        c.dpll = (uint8_t)((intel_rd(r, 0x6c05c) >> (c.ddi * 3 + 1)) & 3u);
        const struct intel_vbt_child *ch = intel_vbt_child_for_ddi(&g.vbt, c.ddi);
        c.aux_ch = ch && ch->aux_ch != INTEL_NO_AUX ? ch->aux_ch : c.ddi;
        g.hw.dpll_users[c.dpll]++;
        intel_hw_disable(&g.hw, &c, 0);
        kprintf("intel-native: the firmware's picture on DDI %c (pipe %c) turned off; it joins the desktop when found\n",
                'A' + c.ddi, 'A' + t);
    }
}

/* The firmware's eDP pipe, read back so it can be turned off cleanly. */
static void read_firmware_edp(void)
{
    const struct intel_regs *r = &g.regs;
    uint32_t conf = intel_rd(r, 0x7f008), fn = intel_rd(r, 0x6f400);
    if (!(conf >> 31) || !(fn >> 31)) return;
    struct intel_crtc_cfg *c = &EDP->cfg;
    memset(c, 0, sizeof *c);
    unsigned in = (fn >> 12) & 7u;
    c->pipe = in == 5 ? 1 : in == 6 ? 2 : 0;
    c->transcoder = INTEL_TRANS_EDP;
    c->ddi = 0;
    c->kind = INTEL_OUT_EDP;
    c->aux_ch = EDP->aux_ch;
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
    EDP->lit = 1;
    EDP->cfg_complete = 0;
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
    struct output *o = EDP;
    const struct display_mode *n = &o->native;
    if (w > n->hdisplay || h > n->vdisplay) return -EMBK_ERANGE;   /* the panel fitter scales up, never down */
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
    struct output *o = EDP;
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
    g.bus = p->bus; g.dev = p->dev; g.fn = p->fn;
    intel_regs_init_mmio(&g.regs, p->mmio);
    g.aperture = p->aperture;
    g.aperture_size = p->aperture_size;
    g.gtt_phys = p->bar0 + p->bar0_size / 2;
    g.gtt_window = p->bar0_size / 2;
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
    probe_ports();
    firmware_externals_off();
    if (!EDP->native.clock_khz && EDP->lit) EDP->native = EDP->cfg.timing;
    if (!EDP->native.clock_khz || !display_mode_sane(&EDP->native)) {
        kprintf("intel-native: no usable native timing for the panel\n");
        return -EMBK_ENODEV;
    }
    g.ready = 1;
    kprintf("intel-native: %04x, VBT %s (BDB %u, %u outputs), panel %s %ux%u %u.%03u MHz, DPCD %u.%u%s, "
            "CDCLK %u kHz, memory latencies %u/%u/%u/%u/%u/%u/%u/%u us\n",
            p->device_id, g.vbt.valid ? "ok" : "absent", g.vbt.bdb_version, g.vbt.child_count,
            EDP->has_identity ? EDP->identity.name : "(no EDID)", EDP->native.hdisplay, EDP->native.vdisplay,
            EDP->native.clock_khz / 1000, EDP->native.clock_khz % 1000, EDP->dpcd[0] >> 4, EDP->dpcd[0] & 0xf,
            EDP->caps.rate_table ? " (rate table)" : "", g.hw.cdclk_khz,
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
    const struct display_mode *n = &EDP->native;
    const struct intel_crtc_cfg *c = &EDP->cfg;
    uint32_t diff = c->timing.clock_khz > n->clock_khz ? c->timing.clock_khz - n->clock_khz : n->clock_khz - c->timing.clock_khz;
    if (EDP->lit && c->src_w == n->hdisplay && c->src_h == n->vdisplay && c->timing.hdisplay == n->hdisplay &&
        c->timing.vdisplay == n->vdisplay && diff * 200u <= n->clock_khz && !c->pfit_enabled) {
        kprintf("intel-native: the firmware's mode is the panel's own; keeping it\n");
        return 0;
    }
    kprintf("intel-native: the firmware left %ux%u%s; setting the panel's %ux%u\n", c->src_w, c->src_h,
            EDP->lit ? "" : " (or nothing)", n->hdisplay, n->vdisplay);
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
    /* A surface may be a whole desktop over several monitors; what each
     * output can scan of it is checked when it is shown (select/apply_layout).
     * 8192 x 4320: a linear plane's 32 KiB stride, and display/layout.h's cap. */
    if (bytes != (uint64_t)pitch * h || !w || !h || w > 8192 || h > 4320) return false;
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
    if (EDP->lit && EDP->cfg.surf_ggtt == g.fb[s].ggtt) return false;   /* still on screen */
    if ((int)s == g.boot_slot) g.boot_slot = -1;
    intel_compose_forget_front(g.fb[s].ggtt);                  /* the blitter stops writing it first */
    fb_free(s);
    return true;
}

void intel_native_describe(struct embk_monitor_info *info)
{
    info->flags |= EMBK_MONITOR_CONNECTED;
    if (!g.ready) return;
    info->flags |= EMBK_MONITOR_MODESET;
    if (EDP->has_identity) display_identity_describe(&EDP->identity, info);
    const struct display_mode *n = &EDP->native;
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

/* ---- several outputs ---------------------------------------------------------- */
unsigned intel_native_output_count(void)
{
    return g.ready ? g.nout : 0;
}

bool intel_native_output_info(unsigned i, struct gpu_output *out)
{
    if (!g.ready || i >= g.nout) return false;
    const struct output *o = &g.out[i];
    memset(out, 0, sizeof *out);
    memcpy(out->name, o->name, sizeof out->name);
    if (o->has_identity) {
        out->has_edid = 1;
        strncpy(out->model, o->identity.name, sizeof out->model - 1);
        memcpy(out->manufacturer, o->identity.manufacturer, sizeof out->manufacturer);
        out->width_mm = o->identity.width_mm;
        out->height_mm = o->identity.height_mm;
    }
    if (i == 0) {
        /* The panel: its own mode, and the smaller ones the panel fitter scales up. */
        static const uint16_t sizes[][2] = { {2560,1440},{1920,1200},{1920,1080},{1680,1050},{1600,900},{1440,900},
                                             {1366,768},{1280,1024},{1280,800},{1280,720},{1024,768},{800,600} };
        const struct display_mode *n = &o->native;
        uint32_t hz = display_mode_refresh_mhz(n);
        out->builtin = 1;
        out->connected = 1;
        out->modes[out->mode_count++] = (struct gpu_output_mode){ n->hdisplay, n->vdisplay, hz, 1 };
        for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0] && out->mode_count < GPU_OUTPUT_MODES; k++) {
            struct intel_pfit pf;
            if (sizes[k][0] > n->hdisplay || sizes[k][1] > n->vdisplay ||
                (sizes[k][0] == n->hdisplay && sizes[k][1] == n->vdisplay) ||
                intel_pfit_compute(sizes[k][0], sizes[k][1], n->hdisplay, n->vdisplay, &pf)) continue;
            out->modes[out->mode_count++] = (struct gpu_output_mode){ sizes[k][0], sizes[k][1], hz, 0 };
        }
        return true;
    }
    out->connected = o->connected;
    for (unsigned k = 0; o->connected && k < o->nmodes; k++)
        out->modes[out->mode_count++] = (struct gpu_output_mode){ o->modes[k].hdisplay, o->modes[k].vdisplay,
                                                                  display_mode_refresh_mhz(&o->modes[k]), k == 0 };
    return true;
}

/* Look at every port's hotplug line; ask whatever is newly there what it is.
 * A monitor that answers slowly after its line rises gets three looks, a
 * second apart. 1 when the set of connected monitors changed. */
int intel_native_poll_outputs(void)
{
    if (!g.ready) return 0;
    int changed = !g.polled;
    g.polled = 1;
    for (unsigned i = 1; i < g.nout; i++) {
        struct output *o = &g.out[i];
        int live = intel_hw_hpd_live(&g.hw, o->ddi);
        if (live != o->hpd) { o->hpd = (uint8_t)live; o->retries = live ? 3 : 0; }
        if (!live) {
            if (o->connected) { o->connected = 0; changed = 1; kprintf("intel-native: %s: unplugged\n", o->name); }
            continue;
        }
        if (o->connected || !o->retries) continue;
        o->retries--;
        if (detect(o)) {
            o->connected = 1; o->retries = 0; changed = 1;
            kprintf("intel-native: %s: %s %s, %ux%u at %u.%03u MHz, %u modes\n", o->name,
                    o->sink == INTEL_OUT_DP ? "DisplayPort" : o->sink == INTEL_OUT_HDMI ? "HDMI" : "DVI",
                    o->has_identity ? o->identity.name : "(unnamed)", o->native.hdisplay, o->native.vdisplay,
                    o->native.clock_khz / 1000, o->native.clock_khz % 1000, o->nmodes);
        } else if (!o->retries) {
            kprintf("intel-native: %s: something is plugged in but does not say what it is\n", o->name);
        }
    }
    return changed;
}

/* Where a pipe's rectangle starts in a linear surface: the 256 KiB-aligned
 * part in PLANE_SURF, the rest in PLANE_OFFSET (i915
 * intel_compute_aligned_offset, linear). */
static void plane_origin(struct intel_crtc_cfg *c, uint32_t surf, uint32_t pitch, int32_t x, int32_t y)
{
    uint32_t off = (uint32_t)y * pitch + (uint32_t)x * 4u, base = off & ~(GGTT_ALIGN - 1u), rest = off - base;
    c->surf_ggtt = surf + base;
    c->stride = pitch;
    c->plane_y = (uint16_t)(rest / pitch);
    c->plane_x = (uint16_t)((rest % pitch) / 4u);
}

/* An external monitor showing w x h from pipe `pipe`. */
static int build_ext_cfg(const struct output *o, uint32_t w, uint32_t h, unsigned pipe, struct intel_crtc_cfg *c)
{
    const struct display_mode *m = 0;
    for (unsigned k = 0; k < o->nmodes && !m; k++)
        if (o->modes[k].hdisplay == w && o->modes[k].vdisplay == h) m = &o->modes[k];
    if (!m) return -EMBK_EINVAL;
    memset(c, 0, sizeof *c);
    c->pipe = (uint8_t)pipe;
    c->transcoder = (uint8_t)pipe;
    c->ddi = o->ddi;
    c->kind = o->sink;
    c->aux_ch = o->aux_ch;
    c->dpll = 0xff;                                     /* assigned with the others */
    c->timing = *m;
    c->src_w = (uint16_t)w;
    c->src_h = (uint16_t)h;
    c->bpc = 8;
    c->keep_firmware_trans = 1;
    if (o->sink == INTEL_OUT_DP) {
        struct intel_dp_source src;
        ext_source(o, &src);
        if (intel_dp_pick_link(&o->caps, &src, m, 0, 24, 0, 0, &c->link)) return -EMBK_ERANGE;
        c->enhanced_framing = o->caps.enhanced_framing;
        c->downspread = o->caps.downspread;
        c->tps3 = o->caps.tps3;
        c->training_interval = o->caps.training_interval;
        c->dpcd_rev = o->dpcd[0];
        c->iboost = o->iboost;
    } else {
        if (intel_wrpll_compute(m->clock_khz, &c->wrpll)) return -EMBK_ERANGE;
        c->hdmi_sink = o->sink == INTEL_OUT_HDMI;
        c->vic = m->vic;
        c->hdmi_level = o->hdmi_level;
        c->iboost = o->hdmi_iboost;
    }
    return 0;
}

/* The same output scanning the same timing on the same port configuration:
 * only its plane (surface, offset, display buffer share) may differ. */
static int same_pipe(const struct output *o, const struct intel_crtc_cfg *a, const struct intel_crtc_cfg *b)
{
    if (!same_scanout(a, b) || a->ddi != b->ddi || a->kind != b->kind) return 0;
    if (o != EDP && a->kind == INTEL_OUT_DP) return a->link.rate == b->link.rate && a->link.lanes == b->link.lanes;
    if (o != EDP) return a->wrpll.cfgcr1 == b->wrpll.cfgcr1 && a->wrpll.cfgcr2 == b->wrpll.cfgcr2;
    return 1;
}

static void source_of(const struct output *o, struct intel_dp_source *src)
{
    if (o == EDP) edp_source(src); else ext_source(o, src);
}

static int light(struct output *o, struct intel_crtc_cfg *c)
{
    struct intel_dp_source src;
    source_of(o, &src);
    int dp = c->kind == INTEL_OUT_DP || c->kind == INTEL_OUT_EDP;
    int rc = intel_hw_enable(&g.hw, c, dp ? &o->caps : 0, dp ? &src : 0);
    if (rc) return rc;
    o->cfg = *c;
    o->lit = 1;
    o->cfg_complete = 1;
    return 0;
}

/* Show a layout: one surface, and the rectangle of it each output scans out.
 *
 * The order is what keeps every lit pipe fed: outputs that go dark, or must
 * be relit differently, are turned off first; pipes that stay lit then take
 * their new (smaller or larger) share of the display buffer -- before any new
 * pipe is given blocks someone else still holds; CDCLK rises (with the pipes
 * off, as it must) when a new mode needs it; then the new outputs light. If
 * any step fails, everything goes back to the previous layout's picture,
 * which still has its surface (the caller frees that only on success). */
bool intel_native_apply_layout(const struct gpu_target *t, const struct gpu_layout *l)
{
    if (!g.ready || !l || l->count > g.nout) return false;
    uint32_t surf;
    if (t->resource_id) {
        unsigned slot = t->resource_id - 1u;
        if (slot >= MAX_FBS || !g.fb[slot].used) return false;
        surf = g.fb[slot].ggtt;
    } else {
        surf = g.fw_ggtt;
    }
    uint32_t pitch = t->mode.pitch;

    struct { int on, stays; struct intel_crtc_cfg cfg; } want[MAX_OUT];
    memset(want, 0, sizeof want);
    unsigned lit_count = 0;
    int pipe_taken[NPIPES] = { 0 }, keep_pipe[MAX_OUT];
    /* The panel always uses pipe A; a monitor keeps the pipe it has. */
    for (unsigned i = 0; i < l->count; i++) {
        if (!l->out[i].enabled) continue;
        if (++lit_count > NPIPES) { kprintf("intel-native: %u outputs at once; this display engine has %u pipes\n", lit_count, NPIPES); return false; }
        if (i > 0 && !g.out[i].connected) return false;
        want[i].on = 1;
    }
    if (!lit_count) return false;
    if (want[0].on) pipe_taken[0] = 1;
    for (unsigned i = 0; i < MAX_OUT; i++) keep_pipe[i] = -1;
    for (unsigned i = 1; i < g.nout; i++)
        if (want[i].on && g.out[i].lit && !pipe_taken[g.out[i].cfg.pipe]) {
            keep_pipe[i] = g.out[i].cfg.pipe;
            pipe_taken[g.out[i].cfg.pipe] = 1;
        }
    for (unsigned i = 0; i < g.nout; i++) {
        if (!want[i].on) continue;
        const struct gpu_output_cfg *oc = &l->out[i];
        struct intel_crtc_cfg *c = &want[i].cfg;
        int rc;
        if (i == 0) {
            rc = build_cfg(oc->width, oc->height, surf, pitch, c);
        } else {
            unsigned pipe = keep_pipe[i] >= 0 ? (unsigned)keep_pipe[i] : NPIPES;
            for (unsigned p = 1; pipe == NPIPES && p <= NPIPES; p++)            /* B, C, then A when the panel is off */
                if (!pipe_taken[p % NPIPES]) { pipe = p % NPIPES; pipe_taken[pipe] = 1; }
            if (pipe == NPIPES) return false;
            rc = build_ext_cfg(&g.out[i], oc->width, oc->height, pipe, c);
        }
        if (rc) { kprintf("intel-native: %s cannot show %ux%u here\n", g.out[i].name, oc->width, oc->height); return false; }
        plane_origin(c, surf, pitch, oc->x, oc->y);
    }
    /* PLLs: the panel has DPLL0; a DP monitor shares a PLL already at its link
     * rate, else takes a free one; an HDMI monitor takes its own. */
    for (unsigned i = 1; i < g.nout; i++) {
        struct intel_crtc_cfg *c = &want[i].cfg;
        if (!want[i].on) continue;
        if (g.out[i].lit && same_pipe(&g.out[i], &g.out[i].cfg, c)) { c->dpll = g.out[i].cfg.dpll; continue; }
    }
    for (unsigned i = 1; i < g.nout; i++) {
        struct intel_crtc_cfg *c = &want[i].cfg;
        if (!want[i].on || c->dpll != 0xff) continue;
        for (unsigned k = 1; k < g.nout && c->dpll == 0xff; k++)
            if (k != i && want[k].on && want[k].cfg.dpll != 0xff && c->kind == INTEL_OUT_DP &&
                want[k].cfg.kind == INTEL_OUT_DP && want[k].cfg.link.rate == c->link.rate) c->dpll = want[k].cfg.dpll;
        for (unsigned id = 1; id < 4 && c->dpll == 0xff; id++) {
            int used = 0;
            for (unsigned k = 1; k < g.nout; k++) if (k != i && want[k].on && want[k].cfg.dpll == id) used = 1;
            if (!used) c->dpll = (uint8_t)id;
        }
        if (c->dpll == 0xff) { kprintf("intel-native: no display PLL left for %s\n", g.out[i].name); return false; }
    }
    /* The display buffer, split by width between the lit pipes in pipe order,
     * and each pipe's watermarks within its share. */
    unsigned order[NPIPES], widths[NPIPES], n = 0;
    for (unsigned p = 0; p < NPIPES; p++)
        for (unsigned i = 0; i < g.nout; i++)
            if (want[i].on && want[i].cfg.pipe == p) { order[n] = i; widths[n] = want[i].cfg.src_w; n++; }
    struct intel_ddb plane[NPIPES], cursor[NPIPES];
    intel_ddb_split(n, widths, plane, cursor);
    uint32_t need_cdclk = 0;
    for (unsigned k = 0; k < n; k++) {
        struct intel_crtc_cfg *c = &want[order[k]].cfg;
        c->plane_ddb = plane[k];
        c->cursor_ddb = cursor[k];
        memset(c->wm, 0, sizeof c->wm);
        if (intel_plane_wm(g.lat, c->timing.clock_khz, c->timing.htotal, c->src_w, 4,
                           (uint32_t)(c->plane_ddb.end - c->plane_ddb.start), c->wm) < 0) {
            kprintf("intel-native: %s at %ux%u does not fit its share of the display buffer\n", g.out[order[k]].name, c->src_w, c->src_h);
            return false;
        }
        if (c->timing.clock_khz > need_cdclk) need_cdclk = c->timing.clock_khz;
    }
    uint32_t cdclk = need_cdclk > g.hw.cdclk_khz ? intel_cdclk_pick(need_cdclk, g.hw.vco_khz) : 0;
    if (need_cdclk > g.hw.cdclk_khz && !cdclk) return false;

    /* The previous picture, to put back if this one fails. */
    struct { int lit, complete; struct intel_crtc_cfg cfg; } before[MAX_OUT];
    for (unsigned i = 0; i < g.nout; i++) {
        before[i].lit = g.out[i].lit; before[i].complete = g.out[i].cfg_complete; before[i].cfg = g.out[i].cfg;
    }

    for (unsigned i = 0; i < g.nout; i++) {
        struct output *o = &g.out[i];
        want[i].stays = want[i].on && o->lit && !cdclk && same_pipe(o, &o->cfg, &want[i].cfg);
        if (o->lit && !want[i].stays) {
            intel_hw_disable(&g.hw, &o->cfg, i == 0 && want[0].on);       /* the panel keeps its power if it stays */
            o->lit = 0;
        }
    }
    for (unsigned k = 0; k < n; k++) {                                   /* new shares, before new pipes */
        struct output *o = &g.out[order[k]];
        if (!want[order[k]].stays) continue;
        struct intel_crtc_cfg run = o->cfg;
        const struct intel_crtc_cfg *c = &want[order[k]].cfg;
        run.surf_ggtt = c->surf_ggtt; run.stride = c->stride; run.plane_x = c->plane_x; run.plane_y = c->plane_y;
        run.plane_ddb = c->plane_ddb; run.cursor_ddb = c->cursor_ddb;
        memcpy(run.wm, c->wm, sizeof run.wm);
        intel_hw_plane_update(&g.hw, &run);
        o->cfg = run;
    }
    int failed = 0;
    if (cdclk && intel_hw_set_cdclk(&g.hw, cdclk)) {
        kprintf("intel-native: CDCLK would not rise to %u kHz: %s\n", cdclk, g.hw.last_error);
        failed = 1;
    }
    for (unsigned k = 0; k < n && !failed; k++) {
        struct output *o = &g.out[order[k]];
        if (o->lit) continue;
        struct intel_crtc_cfg c = want[order[k]].cfg;
        if (light(o, &c)) {
            kprintf("intel-native: %s %ux%u failed: %s\n", o->name, c.src_w, c.src_h, g.hw.last_error);
            failed = 1;
        }
    }
    if (failed) {
        for (unsigned i = 0; i < g.nout; i++)
            if (g.out[i].lit) { intel_hw_disable(&g.hw, &g.out[i].cfg, i == 0); g.out[i].lit = 0; }
        for (unsigned i = 0; i < g.nout; i++) {
            if (!before[i].lit) continue;
            struct intel_crtc_cfg c = before[i].cfg;
            /* The firmware's own configuration was only read back: rebuild it whole. */
            if (i == 0 && !before[0].complete && build_cfg(c.src_w, c.src_h, c.surf_ggtt, c.stride, &c)) continue;
            if (light(&g.out[i], &c)) kprintf("intel-native: %s could not be put back either: %s\n", g.out[i].name, g.hw.last_error);
        }
        return false;
    }
    for (unsigned k = 0; k < n; k++) {
        const struct output *o = &g.out[order[k]];
        const struct intel_crtc_cfg *c = &o->cfg;
        if (intel_hw_underrun(&g.hw, c->pipe))
            kprintf("intel-native: pipe %c underran -- its display buffer share or watermarks are too small\n", 'A' + c->pipe);
        kprintf("intel-native: %s %ux%u at +%u+%u on pipe %c%s\n", o->name, c->src_w, c->src_h,
                l->out[order[k]].x, l->out[order[k]].y, 'A' + c->pipe, want[order[k]].stays ? " (plane moved)" : "");
    }
    return true;
}

/* ---- composing with the blitter ----------------------------------------------
 * The compositor asks once it draws (gpu_driver.compose). The engine gets the
 * GGTT entries mapped once -- sized from GMCH_CTRL, as i915 sizes them -- and
 * its own pages placed ABOVE the aperture: the GPU reaches all of the GGTT,
 * the CPU only the aperture, which stays for framebuffers. Frames go to the
 * surface on screen, whichever fb_select_* made current. */
static volatile uint64_t *g_gsm;
static uint32_t g_gsm_entries, g_blt_next;

static uint64_t gm_page_alloc(void *c)
{
    (void)c;
    uint64_t p = pmm_alloc_page();
    if (p) memset((void *)(uintptr_t)P2V(p), 0, PAGE);
    return p;
}
static void gm_page_free(void *c, uint64_t p) { (void)c; pmm_free_page(p); }
static void *gm_page_kva(void *c, uint64_t p) { (void)c; return (void *)(uintptr_t)P2V(p); }
/* Through a walk that knows huge pages: the kernel image and the direct map
 * are mapped in 2 MiB ones, and the cursor's picture lives in the image. */
static uint64_t gm_va_phys(void *c, const void *va)
{
    (void)c;
    struct vmm_walk_result w;
    vmm_walk(vmm_get_kernel_pml4(), (uint64_t)(uintptr_t)va, &w);
    return w.mapped ? w.phys & ~(uint64_t)(PAGE - 1) : 0;
}
static void gm_ggtt_set(void *c, uint32_t i, uint64_t pte) { (void)c; if (i < g_gsm_entries) g_gsm[i] = pte; }
static uint64_t gm_ggtt_get(void *c, uint32_t i) { (void)c; return i < g_gsm_entries ? g_gsm[i] : 0; }
static uint32_t gm_ggtt_reserve(void *c, uint32_t pages)
{
    (void)c;
    uint64_t at = g_blt_next, end = at + (((uint64_t)pages * PAGE + 0xffffu) & ~0xffffull);
    if (end > (uint64_t)g_gsm_entries * PAGE || end > 0xffffffffull) return 0;
    g_blt_next = (uint32_t)end;
    return (uint32_t)at;
}
static void gm_ggtt_release(void *c, uint32_t ggtt, uint32_t pages)
{
    (void)c;
    uint32_t end = ggtt + ((pages * PAGE + 0xffffu) & ~0xffffu);
    if (end == g_blt_next) g_blt_next = ggtt;               /* the last placed: give it back */
}
static const struct intel_gpu_mem gm_ops = {
    gm_page_alloc, gm_page_free, gm_page_kva, gm_va_phys, gm_ggtt_set, gm_ggtt_get,
    gm_ggtt_reserve, gm_ggtt_release, 0,
};

static int ic_front(void *c, uint32_t *ggtt, uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    (void)c;
    const struct gpu_target *t = fb_current_target();
    if (!t || !t->mode.width || !t->mode.height) return -1;
    if (t->resource_id) {
        unsigned s = t->resource_id - 1u;
        if (s >= MAX_FBS || !g.fb[s].used) return -1;
        *ggtt = g.fb[s].ggtt;
    } else {
        if (!g.fw_bytes) return -1;
        *ggtt = g.fw_ggtt;                                   /* the firmware's own framebuffer */
    }
    *pitch = t->mode.pitch; *w = t->mode.width; *h = t->mode.height;
    return 0;
}
static void *ic_back(void *c, uint32_t *pitch)
{
    (void)c;
    const fb_info_t *fi = fb_get_info();
    *pitch = fi ? fi->pitch : 0;
    return fb_get_buffer();
}
static void *ic_alloc(void *c, uint64_t bytes, void **raw)
{
    (void)c;
    uint8_t *p = (uint8_t *)kmalloc(bytes + PAGE);
    *raw = p;
    return p ? (void *)(((uintptr_t)p + PAGE - 1) & ~(uintptr_t)(PAGE - 1)) : 0;
}
static void ic_free(void *c, void *raw) { (void)c; kfree(raw); }

const struct gpu_compose *intel_native_compose(void)
{
    static int tried;
    static struct intel_compose_env env;
    if (!tried) {
        tried = 1;
        if (!g.ready) return 0;
        uint16_t gmch = pci_read16(g.bus, g.dev, g.fn, 0x50);
        uint32_t ggms = (gmch >> 6) & 3u, gsm = ggms ? (1u << ggms) << 20 : 0;
        if (!gsm || gsm > g.gtt_window) {
            kprintf("intel-native: no GGTT size in GMCH_CTRL (%04x); composing on the CPU\n", gmch);
            return 0;
        }
        g_gsm = (volatile uint64_t *)map_phys(g.gtt_phys, gsm);
        if (!g_gsm) return 0;
        g_gsm_entries = gsm / 8u;
        g_blt_next = (uint32_t)((g.aperture_size + 0xffffu) & ~0xffffull);
        struct intel_blt *b = intel_blt_init(&g.regs, &gm_ops);
        if (!b) return 0;
        env = (struct intel_compose_env){ b, &gm_ops, ic_front, ic_back, ic_alloc, ic_free, 0 };
        if (intel_compose_init(&env)) return 0;
        kprintf("intel-native: composing with the blitter (%u MiB GGTT)\n", g_gsm_entries * PAGE >> 20);
    }
    return intel_compose();
}
