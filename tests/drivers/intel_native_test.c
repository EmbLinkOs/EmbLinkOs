/* intel_native_test.c -- native modesetting's policy and bookkeeping against
 * the register model: inherit the firmware's mode when it is the panel's own,
 * prepare/select/release framebuffers in the GGTT, scale lower modes with the
 * panel fitter, flip when only the surface changes, put the previous mode
 * back when a new one fails, modeset at boot when the firmware's mode is
 * wrong, and describe only what the panel can show. Then several outputs:
 * the VBT's ports, monitors found on their hotplug lines (DP over AUX, HDMI
 * over GMBUS), one surface scanned by up to three pipes, the display buffer
 * split between them in a safe order, mirroring, and a failed layout undone.
 *
 * Built with the kernel's types.h bypassed (-D_TYPES_H and the C library's
 * bool/size_t), because intel_native.c includes kernel headers that macOS's
 * libc disagrees with. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "drivers/video/intel_native.h"
#include "drivers/video/framebuffer.h"
#include "mm/vmm.h"
#include "drivers/video/intel_hw.h"
#include "drivers/video/intel_config.h"
#include "display/edid_modes.h"
#include "../../user/lib/embk_monitor.h"
#include "intel_hwsim.h"
#include "intel_fixtures.h"

/* ---- the kernel services intel_native.c uses ------------------------------ */
#define BAR0      0xf0000000ull
#define GTT       (BAR0 + 0x800000ull)
#define APERTURE  0xd0000000ull
#define APSIZE    0x10000000ull
#define OPREGION  0x8d000000ull
static uint64_t ptes[APSIZE / 4096];
static uint8_t opregion[0x2000];
static unsigned pages_out, pages_in, quiet;
static uint64_t next_page = 0x100000000ull;
static char lastlog[256];

void intel_regs_init_mmio(struct intel_regs *r, volatile void *base) { (void)base; *r = sim; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t o) { (void)b; (void)d; (void)f; return o == 0xfc ? (uint32_t)OPREGION : 0; }
uint64_t pmm_alloc_page(void) { pages_out++; next_page += 4096; return next_page; }
void pmm_free_page(uint64_t p) { (void)p; pages_in++; }
void *kmalloc(uint64_t n) { return malloc(n ? n : 1); }
void kfree(void *p) { free(p); }
/* Composing with the blitter is intel_compose_test.c's; these only link it. */
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t o) { (void)b; (void)d; (void)f; (void)o; return 0; }
uint64_t vmm_get_kernel_pml4(void) { return 0; }
void vmm_walk(uint64_t pml4, uint64_t v, struct vmm_walk_result *w) { (void)pml4; (void)v; memset(w, 0, sizeof *w); }
const struct gpu_target *fb_current_target(void) { return 0; }
const fb_info_t *fb_get_info(void) { return 0; }
uint8_t *fb_get_buffer(void) { return 0; }
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lastlog, sizeof lastlog, fmt, ap);
    va_end(ap);
    if (!quiet) fputs(lastlog, stdout);
}

static struct { uint64_t virt; void *mem; } scratch[16];
static uint64_t map(uint64_t phys, uint64_t size)
{
    if (phys >= OPREGION && phys + size <= OPREGION + sizeof opregion) return (uint64_t)(uintptr_t)(opregion + (phys - OPREGION));
    if (phys >= GTT && phys + size <= GTT + sizeof ptes) return (uint64_t)(uintptr_t)((uint8_t *)ptes + (phys - GTT));
    if (phys >= APERTURE && phys + size <= APERTURE + APSIZE) {          /* a framebuffer being cleared */
        for (unsigned i = 0; i < 16; i++)
            if (!scratch[i].mem) { scratch[i].mem = malloc(size); scratch[i].virt = (uint64_t)(uintptr_t)scratch[i].mem; return scratch[i].virt; }
    }
    fprintf(stderr, "unexpected map %llx+%llx\n", (unsigned long long)phys, (unsigned long long)size);
    abort();
}
uint64_t vmm_map_mmio(uint64_t phys, uint64_t size) { return map(phys, size); }
uint64_t vmm_map_mmio_wc(uint64_t phys, uint64_t size) { return map(phys, size); }
void vmm_unmap_mmio(uint64_t virt, uint64_t size)
{
    (void)size;
    for (unsigned i = 0; i < 16; i++)
        if (scratch[i].mem && scratch[i].virt == virt) { free(scratch[i].mem); scratch[i].mem = 0; }
}

/* ---- the laptop ------------------------------------------------------------- */
static uint8_t edid[256];
static uint32_t conf_writes(void)
{
    uint32_t n = 0;
    for (unsigned i = 0; i < ntrace; i++) if (trace[i].off == 0x7f008) n++;
    return n;
}

static void laptop(unsigned panel_w, unsigned panel_h, uint32_t clock, int firmware_lit, unsigned fw_w, unsigned fw_h)
{
    memset(regs, 0, sizeof regs);
    memset(dpcd, 0, sizeof dpcd);
    memset(ptes, 0, sizeof ptes);
    ntrace = 0; rate_now = 0; sink_dead = 0; sink_gone = 0; need_swing = 1; need_preemph = 1; sink_max_rate = 540000;
    pages_out = pages_in = 0;
    regs[0x46000 / 4] = (2u << 26) | 0x2a1;                     /* CDCLK 337.5 MHz, VCO 8100 */
    regs[0x6c058 / 4] = 1u | (1u << 1);                         /* DPLL0: 2.7 Gbps */
    regs[0x45404 / 4] = (1u << 29) | (1u << 28);
    for (unsigned d = 0; d < 5; d++) regs[(0x64000 + d * 0x100) / 4] = 1u << 7;
    regs[0xc8254 / 4] = 96000; regs[0xc8258 / 4] = 48000;
    regs[0xc7208 / 4] = (2000u << 16) | 500; regs[0xc720c / 4] = (500u << 16) | 500;
    /* the panel */
    dpcd[0] = 0x12; dpcd[1] = 0x0a; dpcd[2] = 0x84; dpcd[3] = 0x01;
    dpcd[0x700] = 0x03;
    put16(dpcd + 0x10, 8100); put16(dpcd + 0x12, 10800); put16(dpcd + 0x14, 13500);
    make_edid(edid, "AUO", 0x40ed, panel_w, panel_h, clock / 10, "B156HAN02.1");
    sink_edid = edid;
    memset(opregion, 0, sizeof opregion);
    memcpy(opregion, "IntelGraphicsMem", 16);
    uint8_t vbt[0x2000];
    uint32_t n = make_vbt(vbt);
    memcpy(opregion + 0x400, vbt, n);
    if (firmware_lit) {
        struct display_mode m;
        struct intel_m_n mn;
        assert(edid_parse_dtd(edid + 54, &m) == 0);
        intel_link_m_n(24, 4, m.clock_khz, 270000, &mn);
        regs[0x7f008 / 4] = (1u << 31) | (1u << 30);
        regs[0x6f400 / 4] = (1u << 31) | (2u << 24) | (3u << 1);
        regs[0x6f000 / 4] = ((uint32_t)(m.htotal - 1) << 16) | (uint32_t)(m.hdisplay - 1);
        regs[0x6f008 / 4] = ((uint32_t)(m.hsync_end - 1) << 16) | (uint32_t)(m.hsync_start - 1);
        regs[0x6f00c / 4] = ((uint32_t)(m.vtotal - 1) << 16) | (uint32_t)(m.vdisplay - 1);
        regs[0x6f014 / 4] = ((uint32_t)(m.vsync_end - 1) << 16) | (uint32_t)(m.vsync_start - 1);
        regs[0x6f040 / 4] = mn.link_m; regs[0x6f044 / 4] = mn.link_n;
        regs[0x6001c / 4] = ((uint32_t)(fw_w - 1) << 16) | (uint32_t)(fw_h - 1);
        if (fw_w != m.hdisplay) regs[0x68180 / 4] = 1u << 31;  /* the firmware scaled */
        regs[0x70180 / 4] = (1u << 31) | (4u << 24);
        regs[0x70188 / 4] = (fw_w * 4 + 63) / 64;
        regs[0x7019c / 4] = 0;
        regs[0x64000 / 4] = (1u << 31) | (1u << 4);
        regs[0xc7200 / 4] = (1u << 31) | 0x8;
        regs[0xc7204 / 4] = 1u | 4u;
        regs[0xc8250 / 4] = 1u << 31;
    }
}

static struct intel_native_probe probe(void)
{
    static uint8_t mmio_dummy;
    static struct gpu_mode fw;
    fw = (struct gpu_mode){ APERTURE, 1920, 1080, 7680, 32, 1 };
    return (struct intel_native_probe){ 0, 2, 0, 0x9b41, INTEL_PCH_CNP, &mmio_dummy, BAR0, 0x1000000, APERTURE, APSIZE, &fw, 0 };
}

static void test_inherit_and_modes(void)
{
    laptop(1920, 1080, 142500, 1, 1920, 1080);
    struct intel_native_probe p = probe();
    assert(intel_native_init(&p) == 0);
    struct gpu_mode m;
    ntrace = 0;
    assert(intel_native_boot_mode(&m) == 0 && conf_writes() == 0);         /* the firmware's mode stays */

    /* 1280x720: a framebuffer in the GGTT, the panel fitter, a real modeset. */
    struct gpu_target t;
    memset(&t, 0, sizeof t);
    uint32_t pitch = intel_native_pitch(1280);
    assert(pitch == 5120);
    assert(intel_native_prepare(&t, 0, (uint64_t)pitch * 720, 1280, 720));
    assert(t.resource_id == 1 && t.mode.pitch == 5120 && t.mode.width == 1280);
    uint32_t ggtt = (uint32_t)(t.mode.phys_addr - APERTURE);
    assert(ggtt % 0x40000 == 0 && ggtt + 5120u * 720u <= APSIZE);
    assert(ptes[ggtt / 4096] & 1 && ptes[ggtt / 4096 + (5120u * 720u - 1) / 4096] & 1 && pages_out == (5120u * 720u + 4095) / 4096);
    ntrace = 0;
    assert(intel_native_select(&t));
    assert(regs[0x6001c / 4] == ((1279u << 16) | 719) && (regs[0x68180 / 4] >> 31));
    assert(regs[0x68174 / 4] == ((1920u << 16) | 1080));                   /* same aspect: full panel */
    assert(regs[0x7019c / 4] == ggtt && regs[0x70188 / 4] == 80 && (regs[0x7f008 / 4] >> 30 & 1));
    assert(regs[0x6f000 / 4] == ((2079u << 16) | 1919));                   /* the panel's own timing */
    assert(((regs[0x6f400 / 4] >> 20) & 7) == 0 && !(regs[0x70030 / 4] & (1u << 4)));   /* EDID's 8 bpc beats the VBT's 6 */
    assert(regs[0xc7200 / 4] >> 31);                                       /* the panel never lost power */

    /* Back to the firmware's own framebuffer at the native mode. */
    struct gpu_target boot = { { APERTURE, 1920, 1080, 7680, 32, 1 }, 0 };
    ntrace = 0;
    assert(intel_native_select(&boot));
    assert(!(regs[0x68180 / 4] >> 31) && regs[0x7019c / 4] == 0 && regs[0x6001c / 4] == ((1919u << 16) | 1079));
    /* Selecting it again is a plane flip: no transcoder is touched. */
    ntrace = 0;
    assert(intel_native_select(&boot) && conf_writes() == 0);

    /* Release: PTEs cleared, pages given back; the scanned-out one is refused. */
    assert(intel_native_release(&t) && !(ptes[ggtt / 4096] & 1) && pages_in == pages_out);
    assert(intel_native_release(&boot));

    /* 1024x768 on 16:9: pillarboxed, centred. */
    memset(&t, 0, sizeof t);
    assert(intel_native_prepare(&t, 0, (uint64_t)intel_native_pitch(1024) * 768, 1024, 768));
    assert(intel_native_select(&t));
    assert(regs[0x68170 / 4] == (240u << 16) && regs[0x68174 / 4] == ((1440u << 16) | 1080));
    assert(!intel_native_release(&t));                                    /* still on screen */
    assert(intel_native_select(&boot) && intel_native_release(&t));

    /* A surface larger than the panel can be prepared -- it may be a desktop
     * over several monitors -- but the panel alone never shows one. */
    assert(intel_native_prepare(&t, 0, (uint64_t)intel_native_pitch(2560) * 1440, 2560, 1440));
    assert(!intel_native_select(&t) && intel_native_release(&t));
    /* The panel's mode preferred, its name shown. */
    struct embk_monitor_info info;
    memset(&info, 0, sizeof info);
    static const unsigned std[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 1024, 768 } };
    for (unsigned i = 0; i < 5; i++) info.modes[info.mode_count++] = (struct embk_monitor_mode){ std[i][0], std[i][1], 0, 0 };
    intel_native_describe(&info);
    assert((info.flags & EMBK_MONITOR_MODESET) && (info.flags & EMBK_MONITOR_EDID) && !strcmp(info.name, "B156HAN02.1"));
    assert(info.mode_count == 3 && info.modes[1].width == 1920 && (info.modes[1].flags & EMBK_MONITOR_PREFERRED));
}

static void test_failure_restores(void)
{
    laptop(1920, 1080, 142500, 1, 1920, 1080);
    struct intel_native_probe p = probe();
    assert(intel_native_init(&p) == 0);
    struct gpu_target t;
    memset(&t, 0, sizeof t);
    assert(intel_native_prepare(&t, 0, (uint64_t)intel_native_pitch(1600) * 900, 1600, 900));
    sink_dead = 1;                                                        /* the link will not train */
    quiet = 1;
    assert(!intel_native_select(&t));
    quiet = 0;
    sink_dead = 0;
    /* The previous mode was put back: but it is only "back" if the link trained
     * again, which it does now that the sink is well. */
    struct gpu_target boot = { { APERTURE, 1920, 1080, 7680, 32, 1 }, 0 };
    assert(intel_native_select(&boot) && regs[0x7019c / 4] == 0 && (regs[0x7f008 / 4] >> 30 & 1));
    assert(intel_native_release(&t));
}

static void test_boot_modeset(void)
{
    /* The firmware scaled 1024x768 onto a 1920x1080 panel: boot sets the panel's own. */
    laptop(1920, 1080, 142500, 1, 1024, 768);
    struct intel_native_probe p = probe();
    assert(intel_native_init(&p) == 0);
    struct gpu_mode m;
    assert(intel_native_boot_mode(&m) == 1);
    assert(m.width == 1920 && m.height == 1080 && m.pitch == 7680 && m.phys_addr > APERTURE);
    assert(!(regs[0x68180 / 4] >> 31) && regs[0x6001c / 4] == ((1919u << 16) | 1079));
    assert(intel_native_resource_id() == 1);
    /* A 1366x768 panel, which nothing lit: its rows are padded to 64 bytes. */
    laptop(1366, 768, 85500, 0, 0, 0);
    p = probe();
    assert(intel_native_init(&p) == 0);
    assert(intel_native_boot_mode(&m) == 1 && m.width == 1366 && m.pitch == 5504 && (regs[0x7f008 / 4] >> 30 & 1));
    assert(regs[0x70188 / 4] == 86 && regs[0x70190 / 4] == ((767u << 16) | 1365));
    /* A panel that does not answer AUX: native modesetting declines, cleanly. */
    laptop(1920, 1080, 142500, 1, 1920, 1080);
    sink_gone = 1;
    quiet = 1;
    p = probe();
    assert(intel_native_init(&p) != 0);
    quiet = 0;
    sink_gone = 0;
}

/* ---- several outputs ----------------------------------------------------- */
#define SDEISR 0xc4000u
static uint8_t dp_edid[256], hdmi_edid[256];

static void fix_sum(uint8_t *b)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 127; i++) sum += b[i];
    b[127] = (uint8_t)(256 - (sum & 0xff));
}

static void plug(unsigned ddi, int on)
{
    static const uint8_t live[4] = { 24, 21, 22, 23 };
    if (on) regs[SDEISR / 4] |= 1u << live[ddi]; else regs[SDEISR / 4] &= ~(1u << live[ddi]);
}

static unsigned written(uint32_t off, uint32_t mask, uint32_t bits)    /* the first write of `bits`, or ~0 */
{
    for (unsigned i = 0; i < ntrace; i++) if (trace[i].off == off && (trace[i].val & mask) == bits) return i;
    return ~0u;
}

static uint32_t surface(struct gpu_target *t, uint32_t w, uint32_t h)
{
    memset(t, 0, sizeof *t);
    assert(intel_native_prepare(t, 0, (uint64_t)intel_native_pitch(w) * h, w, h));
    return (uint32_t)(t->mode.phys_addr - APERTURE);
}

static void test_external_outputs(void)
{
    laptop(1920, 1080, 142500, 1, 1920, 1080);
    ext_mode = 1;
    memset(ext, 0, sizeof ext);
    gmbus_edid = 0; gmbus_sim_pin = 1;
    pcode_reply = 1;
    struct intel_native_probe p = probe();
    assert(intel_native_init(&p) == 0);
    struct gpu_mode m;
    assert(intel_native_boot_mode(&m) == 0);

    /* The fixture's VBT: the panel on DDI A, HDMI on DDI B, DP over USB-C on DDI C. */
    struct gpu_output o;
    assert(intel_native_output_count() == 3);
    assert(intel_native_output_info(0, &o) && !strcmp(o.name, "eDP-1") && o.builtin && o.connected && o.has_edid);
    assert(o.modes[0].width == 1920 && o.modes[0].height == 1080 && o.modes[0].preferred && !strcmp(o.model, "B156HAN02.1"));
    int has720 = 0, has1440 = 0;
    for (unsigned k = 0; k < o.mode_count; k++) {
        has720 |= o.modes[k].width == 1280 && o.modes[k].height == 720;
        has1440 |= o.modes[k].width == 2560;
    }
    assert(has720 && !has1440);                              /* smaller ones, scaled up; nothing larger */
    assert(intel_native_output_info(1, &o) && !strcmp(o.name, "HDMI-1") && !o.connected && !o.mode_count);
    assert(intel_native_output_info(2, &o) && !strcmp(o.name, "DP-1") && !o.connected);
    assert(!intel_native_output_info(3, &o));
    assert(intel_native_poll_outputs() == 1 && intel_native_poll_outputs() == 0);   /* first look; then quiet */

    /* Something on the USB-C port's hotplug line that does not answer: three
     * looks, then it is left alone until the line drops and rises again. */
    plug(2, 1);
    quiet = 1;
    for (int i = 0; i < 5; i++) assert(intel_native_poll_outputs() == 0);
    quiet = 0;
    assert(intel_native_output_info(2, &o) && !o.connected);

    /* A 2560x1440 DisplayPort monitor, also offering 1920x1080. */
    make_edid(dp_edid, "DEL", 0xa0b1, 2560, 1440, 24150, "DELL U2717D");
    dp_edid[38] = 1920 / 8 - 31; dp_edid[39] = 3u << 6;      /* standard timing: 1920x1080 @ 60, 16:9 */
    for (unsigned i = 40; i < 54; i++) dp_edid[i] = 1;
    fix_sum(dp_edid);
    ext[2].present = 1; ext[2].edid = dp_edid;
    ext[2].dpcd[0] = 0x12; ext[2].dpcd[1] = 0x14; ext[2].dpcd[2] = 0x84; ext[2].dpcd[3] = 0x01;
    plug(2, 0); assert(intel_native_poll_outputs() == 0);
    plug(2, 1); assert(intel_native_poll_outputs() == 1);
    assert(intel_native_output_info(2, &o) && o.connected && o.has_edid && !strcmp(o.model, "DELL U2717D"));
    assert(o.modes[0].width == 2560 && o.modes[0].preferred && o.mode_count >= 2 && o.modes[1].width == 1920);

    /* Extend: the panel at the origin, the monitor on its right, one surface. */
    struct gpu_target t;
    uint32_t ggtt = surface(&t, 4480, 1440), pitch = intel_native_pitch(4480);
    struct gpu_layout two = { 3, 0, { { 1, 0, 0, 1920, 1080 }, { 0, 0, 0, 0, 0 }, { 1, 1920, 0, 2560, 1440 } } };
    ntrace = 0;
    assert(intel_native_apply_layout(&t, &two));
    /* The panel kept its pipe and its picture timing: only its plane moved. */
    assert(conf_writes() == 0 && regs[0x7019c / 4] == ggtt && regs[0x70188 / 4] == pitch / 64 && regs[0x701a4 / 4] == 0);
    /* Pipe B, transcoder B, DDI C, DP SST at 2.7 Gbps x4 (the least that carries 241.5 MHz), from DPLL1. */
    uint32_t fn = regs[0x61400 / 4];
    assert((fn >> 31) && ((fn >> 28) & 7) == 2 && ((fn >> 24) & 7) == 2 && ((fn >> 1) & 7) == 3);
    assert(regs[0x71008 / 4] >> 30 & 1);
    assert(((regs[0x6c05c / 4] >> (2 * 3 + 1)) & 3) == 1 && ((regs[0x6c058 / 4] >> (1 * 6 + 1)) & 7) == 1);   /* 2.7: code 1 */
    /* Its plane: the monitor's 2560x1440 rectangle of the surface, from x 1920. */
    assert(regs[0x7119c / 4] == ggtt && regs[0x711a4 / 4] == 1920 && regs[0x71190 / 4] == ((1439u << 16) | 2559));
    /* The display buffer split by width, without overlap; pipe A's share
     * shrank before pipe B was given any. */
    uint32_t a = regs[0x7027c / 4], b = regs[0x7127c / 4];
    assert((a & 0x3ff) == 0 && (a >> 16) + 1 <= (b & 0x3ff) && (b >> 16) < 892 && (b >> 16) - (b & 0x3ff) > (a >> 16) - (a & 0x3ff));
    assert(written(0x7019c, ~0u, ggtt) < written(0x71008, 1u << 31, 1u << 31));

    /* An HDMI television too: three outputs, pipe C, its own PLL, an AVI infoframe. */
    make_edid(hdmi_edid, "SAM", 0x0f00, 1920, 1080, 14850, "SAMSUNG");
    hdmi_edid[126] = 1; fix_sum(hdmi_edid);
    uint8_t *x = hdmi_edid + 128;
    x[0] = 0x02; x[1] = 3; x[2] = 10; x[4] = (3u << 5) | 5; x[5] = 0x03; x[6] = 0x0c; x[7] = 0x00; x[8] = 0x10;
    fix_sum(x);
    gmbus_edid = hdmi_edid;
    plug(1, 1);
    assert(intel_native_poll_outputs() == 1);
    assert(intel_native_output_info(1, &o) && o.connected && !strcmp(o.model, "SAMSUNG") && o.modes[0].width == 1920);
    struct gpu_target t3;
    uint32_t ggtt3 = surface(&t3, 6400, 1440);
    struct gpu_layout three = { 3, 0, { { 1, 0, 0, 1920, 1080 }, { 1, 1920, 0, 1920, 1080 }, { 1, 3840, 0, 2560, 1440 } } };
    ntrace = 0;
    assert(intel_native_apply_layout(&t3, &three));
    fn = regs[0x62400 / 4];
    assert((fn >> 31) && ((fn >> 28) & 7) == 1 && ((fn >> 24) & 7) == 0 && (regs[0x72008 / 4] >> 30 & 1));
    unsigned hdmi_pll = (regs[0x6c05c / 4] >> (1 * 3 + 1)) & 3;
    assert(hdmi_pll == 2 && ((regs[0x6c058 / 4] >> (2 * 6 + 5)) & 1) && regs[0x6c048 / 4] != 0);   /* DPLL2, HDMI mode, CFGCR1 */
    assert(regs[0x62200 / 4] & (1u << 12));
    /* The DP monitor kept pipe B and its link: its plane moved to x 3840, nothing else. */
    assert(written(0x71008, 0, 0) == ~0u && regs[0x7119c / 4] == ggtt3 && regs[0x711a4 / 4] == 3840);
    assert(regs[0x7219c / 4] == ggtt3 && regs[0x721a4 / 4] == 1920);
    uint32_t a3 = regs[0x7027c / 4], b3 = regs[0x7127c / 4], c3 = regs[0x7227c / 4];
    assert((a3 >> 16) < (b3 & 0x3ff) && (b3 >> 16) < (c3 & 0x3ff) && (c3 >> 16) < 892);   /* in pipe order, apart */

    /* A monitor that is not plugged in cannot be lit. */
    plug(1, 0);
    assert(intel_native_poll_outputs() == 1 && intel_native_output_info(1, &o) && !o.connected);
    ntrace = 0;
    assert(!intel_native_apply_layout(&t3, &three) && ntrace == 0);
    /* Back to two: pipe C goes dark and its PLL with it. */
    ntrace = 0;
    assert(intel_native_apply_layout(&t, &two));
    assert(!(regs[0x72008 / 4] >> 30 & 1) && !(regs[0x46040 / 4] >> 31));   /* transcoder C off, DPLL2 off */
    assert(intel_native_release(&t3));

    /* Mirror: both show 1920x1080 from the surface's origin. */
    struct gpu_target tm;
    uint32_t ggm = surface(&tm, 1920, 1080);
    struct gpu_layout mirror = { 3, 0, { { 1, 0, 0, 1920, 1080 }, { 0, 0, 0, 0, 0 }, { 1, 0, 0, 1920, 1080 } } };
    assert(intel_native_apply_layout(&tm, &mirror));
    assert(regs[0x7019c / 4] == ggm && regs[0x7119c / 4] == ggm && regs[0x701a4 / 4] == 0 && regs[0x711a4 / 4] == 0);
    assert(regs[0x71190 / 4] == ((1079u << 16) | 1919) && ((regs[0x6c058 / 4] >> (1 * 6 + 1)) & 7) == 2);   /* 1.62: code 2 */

    /* A monitor that will not train: the new layout fails and the previous
     * picture is put back on the panel. */
    ext[2].dead = 1;
    quiet = 1;
    assert(!intel_native_apply_layout(&t, &two));
    quiet = 0;
    assert((regs[0x7f008 / 4] >> 30 & 1) && regs[0x7019c / 4] == ggm);
    ext[2].dead = 0;
    ext_mode = 0;
}

int main(void)
{
    test_inherit_and_modes();
    test_failure_restores();
    test_boot_modeset();
    test_external_outputs();
    puts("Intel native modesetting: firmware mode inherited, GGTT framebuffers prepared/selected/released, "
         "panel fitter, plane flips, failed modeset restored, boot modeset, 1366-wide panel, describe; "
         "VBT ports, DP and HDMI monitors found by hotplug, three pipes on one surface, display buffer order, "
         "mirroring, a failed layout undone OK");
    return 0;
}
