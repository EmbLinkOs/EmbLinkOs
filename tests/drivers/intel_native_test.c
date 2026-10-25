/* intel_native_test.c -- native modesetting's policy and bookkeeping against
 * the register model: inherit the firmware's mode when it is the panel's own,
 * prepare/select/release framebuffers in the GGTT, scale lower modes with the
 * panel fitter, flip when only the surface changes, put the previous mode
 * back when a new one fails, modeset at boot when the firmware's mode is
 * wrong, and describe only what the panel can show.
 *
 * Built with the kernel's types.h bypassed (-D_TYPES_H and the C library's
 * bool/size_t), because intel_native.c includes kernel headers that macOS's
 * libc disagrees with. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "drivers/video/intel_native.h"
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

    /* Nothing larger than the panel; the panel's mode preferred, its name shown. */
    assert(!intel_native_prepare(&t, 0, (uint64_t)intel_native_pitch(2560) * 1440, 2560, 1440));
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

int main(void)
{
    test_inherit_and_modes();
    test_failure_restores();
    test_boot_modeset();
    puts("Intel native modesetting: firmware mode inherited, GGTT framebuffers prepared/selected/released, "
         "panel fitter, plane flips, failed modeset restored, boot modeset, 1366-wide panel, describe OK");
    return 0;
}
