/* intel_capture.c -- build /EMBKDISP.BIN. See intel_capture.h for what is
 * recorded, why, and the file format; tools/intel_display_dump.py reads it. */
#include "drivers/video/intel_capture.h"
#include "drivers/video/intel_dpaux.h"
#include "drivers/video/intel_gmbus.h"
#include "include/errno.h"

/* ---- the register blocks recorded ---------------------------------------
 * Gen9 display (Skylake to Comet Lake), per Intel's programmer's reference.
 * Each block is read 32 bits at a time; reading a display register has no
 * side effect, with one exception the list steps around: GMBUS3 (0xc510c)
 * is the I2C data FIFO, and reading it consumes a byte. */
static const struct { uint32_t start, end; } mmio_blocks[] = {
    { 0x42000, 0x42100 },   /* fuse status, display chicken bits */
    { 0x44000, 0x44500 },   /* display engine interrupts: pipe, port, misc */
    { 0x45000, 0x45600 },   /* DBUF, power wells, DC states */
    { 0x46000, 0x46100 },   /* CDCLK, LCPLL, WRPLL */
    { 0x51000, 0x51010 },   /* display fuse straps (DFSM) */
    { 0x60000, 0x60100 }, { 0x60400, 0x60500 },   /* transcoder A: timings, M/N, DDI function */
    { 0x61000, 0x61100 }, { 0x61400, 0x61500 },   /* transcoder B */
    { 0x62000, 0x62100 }, { 0x62400, 0x62500 },   /* transcoder C */
    { 0x6f000, 0x6f100 }, { 0x6f400, 0x6f500 },   /* transcoder eDP */
    { 0x64000, 0x64600 },   /* DDI A-E: buffer, AUX, DP transport */
    { 0x64e00, 0x65000 },   /* DDI buffer translation tables */
    { 0x68000, 0x69a00 },   /* pipe scalers A-C */
    { 0x6c000, 0x6c100 },   /* DPLL control and configuration */
    { 0x70000, 0x70600 },   /* pipe A: config, status, planes, cursor, watermarks, DDB */
    { 0x71000, 0x71600 },   /* pipe B */
    { 0x72000, 0x72600 },   /* pipe C */
    { 0x7f000, 0x7f100 },   /* transcoder eDP's own configuration (TRANS_CONF) */
    { 0xc2000, 0xc2100 },   /* south fuse strap: which DDIs exist */
    { 0xc4000, 0xc4100 },   /* south interrupts, hotplug control */
    { 0xc5100, 0xc510c },   /* GMBUS0-2 (not GMBUS3: see above) */
    { 0xc5110, 0xc5124 },   /* GMBUS4-5 */
    { 0xc7200, 0xc7300 },   /* panel power sequencer */
    { 0xc8250, 0xc8300 },   /* backlight PWM */
};

/* DPCD read in 16-byte pieces, each its own section, so a sink that NACKs a
 * reserved address loses that piece and nothing around it. */
static const struct { uint32_t addr, len; } dpcd_blocks[] = {
    { 0x00000, 0x100 },     /* receiver capabilities, eDP link rates, PSR */
    { 0x00100, 0x020 },     /* link configuration */
    { 0x00200, 0x010 },     /* sink and lane status */
    { 0x00600, 0x001 },     /* sink power state */
    { 0x00700, 0x040 },     /* eDP general and backlight capabilities */
    { 0x02200, 0x010 },     /* extended receiver capabilities (DP 1.3+) */
};

#define OPREGION_SIZE      0x2000u
#define OPREGION_SIG       "IntelGraphicsMem"
#define OPREGION_OVER      0x14u
#define OPREGION_MBOX      0x58u
#define OPREGION_ASLE_RVDA 0x3bau
#define OPREGION_ASLE_RVDS 0x3c2u
#define OPREGION_VBT       0x400u
#define MBOX_ASLE          (1u << 2)
#define RVDA_MAX           0x10000u

#define PWR_WELL_CTL2      0x45404u
#define PW2_STATE          (1u << 30)

#define PCODE_MAILBOX      0x138124u
#define PCODE_DATA         0x138128u
#define PCODE_DATA1        0x13812cu
#define PCODE_READY        (1u << 31)
#define PCODE_READ_MEM_LATENCY 0x6u
#define PCODE_WAIT_US      10000u

#define GTT_MAX_PTES       4096u
#define NOTE_RESERVE       64u      /* room kept back for the "truncated" note */
#define PCI_ASLS           0xfcu

struct writer {
    uint8_t *buf;
    uint32_t cap, len;
    uint32_t sections;
    int truncated;
};

static void put8(uint8_t *p, uint8_t v) { p[0] = v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* Reserve a section with a payload of `len` bytes; NULL (and the truncated
 * flag) when it does not fit. The payload is zeroed, padding included. */
static uint8_t *section(struct writer *w, uint32_t tag, uint32_t len)
{
    uint32_t padded = (len + 3u) & ~3u;
    if (w->truncated || w->cap - w->len < 8 + padded) { w->truncated = 1; return 0; }
    uint8_t *h = w->buf + w->len;
    put32(h, tag);
    put32(h + 4, len);
    for (uint32_t i = 0; i < padded; i++) h[8 + i] = 0;
    w->len += 8 + padded;
    w->sections++;
    return h + 8;
}

static void note(struct writer *w, const char *text)
{
    uint32_t n = 0;
    while (text[n]) n++;
    uint8_t *p = section(w, INTEL_CAP_NOTE, n);
    for (uint32_t i = 0; p && i < n; i++) p[i] = (uint8_t)text[i];
}

static uint32_t rd_le32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void capture_pci(struct writer *w, const struct intel_capture_env *env)
{
    for (unsigned i = 0; i < env->function_count && i < 8; i++) {
        const struct intel_capture_bdf *f = &env->functions[i];
        uint8_t *p = section(w, INTEL_CAP_PCI, 4 + 256);
        if (!p) return;
        put8(p, f->bus); put8(p + 1, f->dev); put8(p + 2, f->fn); put8(p + 3, f->role);
        for (unsigned off = 0; off < 256; off += 4)
            put32(p + 4 + off, env->cfg_read32(env->ctx, f->bus, f->dev, f->fn, (uint8_t)off));
    }
}

static void capture_bars_and_boot(struct writer *w, const struct intel_capture_env *env)
{
    uint8_t *p = section(w, INTEL_CAP_BARS, 36);
    if (p) {
        put64(p, env->bar0); put64(p + 8, env->bar0_size);
        put64(p + 16, env->bar2); put64(p + 24, env->bar2_size);
        put32(p + 32, env->pch_layout);
    }
    p = section(w, INTEL_CAP_BOOTFB, 40 + 128);
    if (p) {
        put64(p, env->fb_addr);
        put32(p + 8, env->fb_width); put32(p + 12, env->fb_height);
        put32(p + 16, env->fb_pitch); put32(p + 20, env->fb_bpp);
        put32(p + 24, env->fb_format); put32(p + 28, env->firmware);
        uint32_t n = env->edid && env->edid_size >= 128 ? 128 : 0;
        put32(p + 32, n);
        for (uint32_t i = 0; i < n; i++) p[40 + i] = env->edid[i];
    }
}

static void capture_raw(struct writer *w, const struct intel_capture_env *env, uint32_t tag,
                        uint64_t phys, uint32_t len)
{
    const volatile uint8_t *v = env->map(env->ctx, phys, len);
    if (!v) { note(w, tag == INTEL_CAP_RVDA ? "RVDA: could not map" : "OpRegion: could not map"); return; }
    uint8_t *p = section(w, tag, 12 + len);
    if (p) {
        put64(p, phys);
        put32(p + 8, len);
        for (uint32_t i = 0; i < len; i++) p[12 + i] = v[i];
    }
    env->unmap(env->ctx, v, len);
}

static void capture_opregion(struct writer *w, const struct intel_capture_env *env,
                             struct intel_capture_summary *sum)
{
    uint32_t asls = env->cfg_read32(env->ctx, env->functions[0].bus, env->functions[0].dev,
                                    env->functions[0].fn, PCI_ASLS);
    if (!asls || asls == 0xffffffffu) { note(w, "OpRegion: ASLS is 0, the firmware published none"); return; }

    const volatile uint8_t *v = env->map(env->ctx, asls, OPREGION_SIZE);
    if (!v) { note(w, "OpRegion: could not map"); return; }
    int signed_ok = 1;
    for (unsigned i = 0; i < 16; i++)
        if (v[i] != (uint8_t)OPREGION_SIG[i]) signed_ok = 0;
    uint32_t over = rd_le32(v + OPREGION_OVER);
    uint32_t mbox = rd_le32(v + OPREGION_MBOX);
    uint64_t rvda = (uint64_t)rd_le32(v + OPREGION_ASLE_RVDA) |
                    (uint64_t)rd_le32(v + OPREGION_ASLE_RVDA + 4) << 32;
    uint32_t rvds = rd_le32(v + OPREGION_ASLE_RVDS);
    uint32_t mb4_size = (uint32_t)v[OPREGION_VBT + 24] | (uint32_t)v[OPREGION_VBT + 25] << 8;
    int mb4_vbt = v[OPREGION_VBT] == '$' && v[OPREGION_VBT + 1] == 'V' &&
                   v[OPREGION_VBT + 2] == 'B' && v[OPREGION_VBT + 3] == 'T';
    env->unmap(env->ctx, v, OPREGION_SIZE);

    capture_raw(w, env, INTEL_CAP_OPREGION, asls, OPREGION_SIZE);
    if (!signed_ok) { note(w, "OpRegion: signature is not IntelGraphicsMem"); return; }

    unsigned major = over >> 24, minor = (over >> 16) & 0xffu;
    sum->opregion_version = major << 8 | minor;
    if (major >= 2 && (mbox & MBOX_ASLE) && rvda && rvds) {
        /* From OpRegion 2.1 the raw VBT's address is relative to the OpRegion
         * itself; 2.0 gave it absolute (i915 intel_opregion.c). */
        uint64_t phys = (major > 2 || minor >= 1) ? (uint64_t)asls + rvda : rvda;
        uint32_t len = rvds > RVDA_MAX ? RVDA_MAX : rvds;
        if (rvds > RVDA_MAX) note(w, "RVDA: VBT larger than 64 KiB, truncated");
        capture_raw(w, env, INTEL_CAP_RVDA, phys, len);
        sum->vbt_bytes = len;
    } else if (mb4_vbt) {
        sum->vbt_bytes = mb4_size;
    } else {
        note(w, "OpRegion: no VBT in mailbox 4 and no RVDA");
    }
}

static void capture_mmio(struct writer *w, const struct intel_regs *r)
{
    for (unsigned b = 0; b < sizeof mmio_blocks / sizeof mmio_blocks[0]; b++) {
        uint32_t start = mmio_blocks[b].start, count = (mmio_blocks[b].end - start) / 4;
        uint8_t *p = section(w, INTEL_CAP_MMIO, 8 + count * 4);
        if (!p) return;
        put32(p, start);
        put32(p + 4, count);
        for (uint32_t i = 0; i < count; i++)
            put32(p + 8 + i * 4, intel_rd(r, start + i * 4));
    }
}

static void capture_dpcd_piece(struct writer *w, unsigned ch, uint32_t addr, uint32_t len,
                               int rc, const uint8_t *data)
{
    uint32_t keep = rc == 0 ? len : 0;
    uint8_t *p = section(w, INTEL_CAP_DPCD, 16 + keep);
    if (!p) return;
    put8(p, (uint8_t)ch);
    put32(p + 4, addr);
    put32(p + 8, (uint32_t)rc);
    put32(p + 12, keep);
    for (uint32_t i = 0; i < keep; i++) p[16 + i] = data[i];
}

static void capture_aux(struct writer *w, const struct intel_capture_env *env,
                        struct intel_capture_summary *sum)
{
    const struct intel_regs *r = env->regs;
    /* AUX A lives in power well 1, which is on whenever the display is. AUX
     * B-D need power well 2; when the firmware left it off their registers do
     * not respond, and turning a well on is not something a capture does. */
    sum->aux_bcd_powered = (intel_rd(r, PWR_WELL_CTL2) & PW2_STATE) != 0;
    if (!sum->aux_bcd_powered)
        note(w, "AUX B-D not probed: power well 2 is off");

    for (unsigned ch = 0; ch < INTEL_AUX_CHANNELS; ch++) {
        if (ch != INTEL_AUX_A && !sum->aux_bcd_powered) continue;
        uint8_t data[16];
        int rc = intel_dpcd_read(r, ch, 0, data, sizeof data);
        if (rc) {                       /* no sink: record why, once, and move on */
            capture_dpcd_piece(w, ch, 0, sizeof data, rc, data);
            continue;
        }
        sum->dpcd_rev[ch] = data[0] ? data[0] : 0xff;
        for (unsigned b = 0; b < sizeof dpcd_blocks / sizeof dpcd_blocks[0]; b++) {
            for (uint32_t off = 0; off < dpcd_blocks[b].len; off += 16) {
                uint32_t n = dpcd_blocks[b].len - off < 16 ? dpcd_blocks[b].len - off : 16;
                uint32_t addr = dpcd_blocks[b].addr + off;
                rc = intel_dpcd_read(r, ch, addr, data, n);
                capture_dpcd_piece(w, ch, addr, n, rc, data);
            }
        }
        uint8_t edid[256];
        rc = intel_aux_i2c_read(r, ch, 0x50, 0, edid, sizeof edid);
        uint8_t *p = section(w, INTEL_CAP_AUXEDID, 12 + (rc == 0 ? sizeof edid : 0));
        if (p) {
            put8(p, (uint8_t)ch);
            put32(p + 4, (uint32_t)rc);
            put32(p + 8, rc == 0 ? sizeof edid : 0);
            for (unsigned i = 0; rc == 0 && i < sizeof edid; i++) p[12 + i] = edid[i];
        }
        if (rc == 0) sum->aux_edid |= (uint8_t)(1u << ch);
    }
}

static void capture_gmbus(struct writer *w, const struct intel_capture_env *env,
                          struct intel_capture_summary *sum)
{
    static const uint8_t cnp[] = { 1, 2, 3, 4 }, spt[] = { 4, 5, 6 }, any[] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t *pins = env->pch_layout == INTEL_PCH_CNP ? cnp :
                          env->pch_layout == INTEL_PCH_SPT ? spt : any;
    unsigned n = env->pch_layout == INTEL_PCH_CNP ? sizeof cnp :
                 env->pch_layout == INTEL_PCH_SPT ? sizeof spt : sizeof any;
    for (unsigned i = 0; i < n; i++) {
        uint8_t edid[256];
        int rc = intel_gmbus_read(env->regs, pins[i], 0x50, 0, edid, sizeof edid);
        uint8_t *p = section(w, INTEL_CAP_GMBUSEDID, 12 + (rc == 0 ? sizeof edid : 0));
        if (!p) return;
        put8(p, pins[i]);
        put32(p + 4, (uint32_t)rc);
        put32(p + 8, rc == 0 ? sizeof edid : 0);
        for (unsigned b = 0; rc == 0 && b < sizeof edid; b++) p[12 + b] = edid[b];
        if (rc == 0) sum->gmbus_edid |= (uint8_t)(1u << pins[i]);
    }
}

/* A pcode mailbox read (i915 snb_pcode_rw): the watermark algorithm needs the
 * DRAM latencies only the power controller knows. */
static void capture_pcode(struct writer *w, const struct intel_regs *r, uint32_t cmd, uint32_t param)
{
    int rc = 0;
    uint32_t value = 0;
    if (intel_rd(r, PCODE_MAILBOX) & PCODE_READY) {
        rc = -EMBK_EBUSY;                           /* someone else's request is pending */
    } else {
        intel_wr(r, PCODE_DATA, param);
        intel_wr(r, PCODE_DATA1, 0);
        intel_wr(r, PCODE_MAILBOX, PCODE_READY | cmd);
        uint64_t deadline = r->now_us(r->ctx) + PCODE_WAIT_US;
        while ((intel_rd(r, PCODE_MAILBOX) & PCODE_READY) && r->now_us(r->ctx) < deadline) { }
        if (intel_rd(r, PCODE_MAILBOX) & PCODE_READY) rc = -EMBK_ETIMEDOUT;
        else if (intel_rd(r, PCODE_MAILBOX) & 0xffu) rc = -EMBK_EIO;   /* pcode refused it */
        else value = intel_rd(r, PCODE_DATA);
    }
    uint8_t *p = section(w, INTEL_CAP_PCODE, 16);
    if (!p) return;
    put32(p, cmd); put32(p + 4, param); put32(p + 8, (uint32_t)rc); put32(p + 12, value);
}

static void capture_gtt(struct writer *w, const struct intel_capture_env *env)
{
    if (env->bar0_size < 0x1000000u || !env->bar2 || env->fb_addr < env->bar2 ||
        env->fb_addr - env->bar2 >= env->bar2_size) {
        note(w, "GTT: the boot framebuffer is not inside the aperture");
        return;
    }
    uint64_t gtt = env->bar0 + env->bar0_size / 2;   /* Gen8+: the upper half of BAR0 */
    uint32_t first = (uint32_t)((env->fb_addr - env->bar2) >> 12);
    uint64_t bytes = (uint64_t)env->fb_pitch * env->fb_height;
    uint32_t pages = (uint32_t)((bytes + 4095) >> 12);
    uint32_t count = pages < GTT_MAX_PTES ? pages : GTT_MAX_PTES;
    const volatile uint8_t *v = env->map(env->ctx, gtt + (uint64_t)first * 8, count * 8);
    if (!v) { note(w, "GTT: could not map"); return; }
    uint8_t *p = section(w, INTEL_CAP_GTT, 20 + count * 8);
    if (p) {
        put64(p, gtt);
        put32(p + 8, first);
        put32(p + 12, pages);
        put32(p + 16, count);
        for (uint32_t i = 0; i < count; i++) {
            put32(p + 20 + i * 8, rd_le32(v + i * 8));
            put32(p + 24 + i * 8, rd_le32(v + i * 8 + 4));
        }
    }
    env->unmap(env->ctx, v, count * 8);
}

uint32_t intel_capture_build(const struct intel_capture_env *env, uint8_t *buf,
                             uint32_t cap, struct intel_capture_summary *sum)
{
    struct intel_capture_summary scratch;
    if (!sum) sum = &scratch;
    for (unsigned i = 0; i < sizeof *sum; i++) ((uint8_t *)sum)[i] = 0;
    if (!buf || cap < 16 + NOTE_RESERVE || !env || !env->function_count) return 0;

    struct writer w = { buf, cap - NOTE_RESERVE, 16, 0, 0 };
    for (unsigned i = 0; i < 8; i++) buf[i] = (uint8_t)"EMBKDISP"[i];
    put32(buf + 8, INTEL_CAPTURE_VERSION);

    capture_pci(&w, env);
    capture_bars_and_boot(&w, env);
    capture_opregion(&w, env, sum);
    capture_mmio(&w, env->regs);
    capture_aux(&w, env, sum);
    capture_gmbus(&w, env, sum);
    capture_pcode(&w, env->regs, PCODE_READ_MEM_LATENCY, 0);
    capture_pcode(&w, env->regs, PCODE_READ_MEM_LATENCY, 1);
    capture_gtt(&w, env);
    if (w.truncated) {
        /* Say so in the file itself, in space kept back for exactly this. */
        w.truncated = 0;
        w.cap = cap;
        note(&w, "capture truncated: buffer full");
        sum->truncated = 1;
    }
    put32(buf + 12, w.sections);
    sum->sections = w.sections;
    return w.len;
}

#ifndef INTEL_CAPTURE_HOST
/* ---- the kernel side: find the hardware, build, leave it on the stick ---- */
#include "drivers/video/intel_mmio.h"
#include "drivers/video/intel_display.h"
#include "drivers/bus/pci.h"
#include "boot/boot_protocol.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "lib/bootlog.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/timer/timer.h"

static uint32_t cfg_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)ctx;
    return pci_read32(bus, dev, fn, off);
}

/* Firmware memory (OpRegion, VBT) and the GTT are both read uncached through
 * the MMIO window -- the way the AML interpreter maps OperationRegions. */
static const volatile uint8_t *map_phys(void *ctx, uint64_t phys, uint32_t len)
{
    (void)ctx;
    uint64_t base = phys & ~0xfffull, span = (phys - base) + len;
    uint64_t v = vmm_map_mmio(base, span);
    return v ? (const volatile uint8_t *)(uintptr_t)(v + (phys - base)) : 0;
}

static void unmap_phys(void *ctx, const volatile uint8_t *virt, uint32_t len)
{
    (void)ctx;
    uint64_t v = (uint64_t)(uintptr_t)virt, base = v & ~0xfffull;
    vmm_unmap_mmio(base, (v - base) + len);
}

void intel_capture_run(void)
{
    const struct pci_device *gpu = 0;
    struct intel_capture_env env;
    memset(&env, 0, sizeof env);
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->class_code != 3) continue;
        if (!gpu && d->vendor_id == 0x8086) gpu = d;
    }
    if (!gpu) return;

    env.functions[env.function_count++] =
        (struct intel_capture_bdf){ gpu->bus, gpu->device, gpu->function, 0 };
    for (uint32_t i = 0; i < pci_devices_count() && env.function_count < 8; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d == gpu) continue;
        if (d->vendor_id == 0x8086 && d->class_code == 6 && d->subclass == 1)
            env.functions[env.function_count++] =
                (struct intel_capture_bdf){ d->bus, d->device, d->function, 1 };
        else if (d->class_code == 3)     /* a discrete GPU beside it, e.g. NVIDIA Optimus */
            env.functions[env.function_count++] =
                (struct intel_capture_bdf){ d->bus, d->device, d->function, 2 };
    }

    /* A function whose memory decoder the firmware left off is not ours to
     * wake: its registers would not answer anyway. */
    if (!(pci_read16(gpu->bus, gpu->device, gpu->function, PCI_COMMAND) & 2)) {
        kprintf("intel-capture: %04x has memory decoding off; not captured\n", gpu->device_id);
        return;
    }
    struct pci_bar bar0 = pci_read_bar(gpu->bus, gpu->device, gpu->function, 0);
    struct pci_bar bar2 = pci_read_bar(gpu->bus, gpu->device, gpu->function, 2);
    if (!bar0.valid || !bar0.is_mmio || bar0.size < INTEL_CAPTURE_REGS) {
        kprintf("intel-capture: %04x BAR0 is not a usable register BAR; not captured\n",
                gpu->device_id);
        return;
    }
    volatile uint8_t *mmio = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar0.address, INTEL_CAPTURE_REGS);
    if (!mmio) { kprintf("intel-capture: could not map BAR0\n"); return; }
    struct intel_regs regs;
    intel_regs_init_mmio(&regs, mmio);

    env.regs = &regs;
    env.cfg_read32 = cfg_read32;
    env.map = map_phys;
    env.unmap = unmap_phys;
    env.bar0 = bar0.address; env.bar0_size = bar0.size;
    if (bar2.valid && bar2.is_mmio) { env.bar2 = bar2.address; env.bar2_size = bar2.size; }
    env.pch_layout = intel_pch_layout();
    const struct boot_protocol *bp = boot_protocol_get();
    if (bp) {
        env.fb_addr = bp->fb_addr; env.fb_width = bp->fb_width; env.fb_height = bp->fb_height;
        env.fb_pitch = bp->fb_pitch; env.fb_bpp = bp->fb_bpp; env.fb_format = bp->fb_format;
        env.firmware = bp->firmware;
        if (bp->size >= BOOT_PROTOCOL_SIZE_V5 && bp->display_edid_size == 128) {
            env.edid_size = 128;
            env.edid = bp->display_edid;
        }
    }

    uint8_t *buf = (uint8_t *)kmalloc(INTEL_CAPTURE_MAX);
    if (!buf) { vmm_unmap_mmio((uint64_t)(uintptr_t)mmio, INTEL_CAPTURE_REGS); return; }
    uint64_t t0 = time_get_us();
    struct intel_capture_summary sum;
    uint32_t len = intel_capture_build(&env, buf, INTEL_CAPTURE_MAX, &sum);
    uint64_t took = time_get_us() - t0;
    vmm_unmap_mmio((uint64_t)(uintptr_t)mmio, INTEL_CAPTURE_REGS);

    /* Keep only what was written; the stick gets it once the ESP is mounted. */
    uint8_t *kept = len ? (uint8_t *)kmalloc(len) : 0;
    if (kept) memcpy(kept, buf, len);
    kfree(buf);
    if (!kept) { kprintf("intel-capture: out of memory\n"); return; }
    bootlog_attach(INTEL_CAPTURE_PATH, kept, (uint32_t)len);

    kprintf("intel-capture: %04x, OpRegion %u.%u, VBT %u bytes, DPCD A/B/C/D %02x/%02x/%02x/%02x%s, "
            "AUX EDID mask %x, GMBUS EDID mask %x, %u sections, %u bytes in %u us%s -> %s\n",
            gpu->device_id, sum.opregion_version >> 8, sum.opregion_version & 0xff,
            (unsigned)sum.vbt_bytes, sum.dpcd_rev[0], sum.dpcd_rev[1], sum.dpcd_rev[2],
            sum.dpcd_rev[3], sum.aux_bcd_powered ? "" : " (B-D unpowered)",
            sum.aux_edid, sum.gmbus_edid, (unsigned)sum.sections, (unsigned)len,
            (unsigned)took, sum.truncated ? " TRUNCATED" : "", INTEL_CAPTURE_PATH);
}
#endif
