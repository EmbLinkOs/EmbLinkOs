/* intel_capture_test.c -- the AUX, GMBUS and capture code against a simulated
 * Comet Lake laptop: an eDP sink on AUX A (with DEFERs, short replies and a
 * NACKed reserved range), an HDMI monitor's EDID on GMBUS pin 1, pcode, and
 * an OpRegion 2.1 whose VBT sits behind RVDA.
 *
 * With INTEL_CAPTURE_OUT set, the built capture is also written there, for
 * tests/drivers/intel_capture_decode_test.py to decode. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/video/intel_dpaux.h"
#include "drivers/video/intel_gmbus.h"
#include "drivers/video/intel_capture.h"
#include "include/errno.h"

/* ---- the simulated device ------------------------------------------------ */
static uint32_t regs[INTEL_CAPTURE_REGS / 4];
static uint64_t clock_us;
static uint8_t dpcd[0x3000];
static uint8_t edp_edid[256], hdmi_edid[256];
static int sink_on[4] = { 1, 0, 0, 0 };
static int defer_left, nack_reserved = 1, short_replies;
static unsigned i2c_offset, aux_messages;
static uint32_t gmbus2, gmbus_left;
static unsigned gmbus_pos, gmbus_hdmi_pin = 1;
static uint32_t pcode_mailbox, writes_outside_protocol;

#define CTL_BUSY (1u << 31)
#define CTL_DONE (1u << 30)
#define CTL_TIMEOUT (1u << 28)
#define CTL_RECV (1u << 25)

static void aux_reply(unsigned ch, const uint8_t *reply, unsigned n)
{
    for (unsigned i = 0; i < 5; i++) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; b++)
            if (i * 4 + b < n) w |= (uint32_t)reply[i * 4 + b] << ((3 - b) * 8);
        regs[DP_AUX_CH_DATA(ch, i) / 4] = w;
    }
    regs[DP_AUX_CH_CTL(ch) / 4] = CTL_DONE | (n << 20);
}

static void aux_exec(unsigned ch, uint32_t ctl)
{
    aux_messages++;
    unsigned size = (ctl >> 20) & 0x1f;
    assert(size >= 3 && size <= 20);
    assert(!(ctl & (1u << 29)));           /* never asks for an interrupt */
    uint8_t tx[20];
    for (unsigned i = 0; i < size; i++)
        tx[i] = (uint8_t)(regs[DP_AUX_CH_DATA(ch, i / 4) / 4] >> ((3 - i % 4) * 8));
    if (!sink_on[ch]) { regs[DP_AUX_CH_CTL(ch) / 4] = CTL_TIMEOUT; return; }

    unsigned req = tx[0] >> 4;
    uint32_t addr = (uint32_t)(tx[0] & 0xf) << 16 | (uint32_t)tx[1] << 8 | tx[2];
    uint8_t reply[20] = { 0 };
    if (defer_left > 0) { defer_left--; reply[0] = 0x20; aux_reply(ch, reply, 1); return; }
    if (req == 0x9) {                      /* native read */
        assert(size == 4);
        unsigned len = tx[3] + 1u;
        assert(len <= 16 && addr + len <= sizeof dpcd);
        if (nack_reserved && addr >= 0x80 && addr < 0x90) { reply[0] = 0x10; aux_reply(ch, reply, 1); return; }
        if (short_replies && len > 5) len = 5;
        for (unsigned i = 0; i < len; i++) reply[1 + i] = dpcd[addr + i];
        aux_reply(ch, reply, 1 + len);
    } else if (req == 0x8) {               /* native write */
        unsigned len = tx[3] + 1u;
        for (unsigned i = 0; i < len; i++) dpcd[addr + i] = tx[4 + i];
        aux_reply(ch, reply, 1);
    } else {                               /* I2C over AUX: 0/1, with MOT 4/5 */
        unsigned rd = req & 1u, mot = req & 4u;
        if (addr != 0x50) { reply[0] = 0x40; aux_reply(ch, reply, 1); return; }
        if (size == 3) {                   /* address only: start, or stop */
            if (!mot) i2c_offset = 0;
            aux_reply(ch, reply, 1);
        } else if (!rd) {
            i2c_offset = tx[4];
            aux_reply(ch, reply, 1);
        } else {
            unsigned len = tx[3] + 1u;
            if (short_replies && len > 7) len = 7;
            for (unsigned i = 0; i < len; i++) reply[1 + i] = edp_edid[(i2c_offset + i) & 0xff];
            i2c_offset += len;
            aux_reply(ch, reply, 1 + len);
        }
    }
}

#define GMBUS0 0xc5100u
#define GMBUS1 0xc5104u
#define GMBUS2 0xc5108u
#define GMBUS3 0xc510cu

static void gmbus_cmd(uint32_t v)
{
    if (v & (1u << 31)) { gmbus2 = 0; return; }          /* SW_CLR_INT */
    if (!(v & (1u << 30))) return;                        /* not SW_RDY */
    unsigned cycle = (v >> 25) & 7u;
    if (cycle == 4) { gmbus2 = 0; return; }               /* STOP: idle */
    assert(cycle == 3 && (v & 1u));                       /* indexed read, WAIT */
    unsigned pin = regs[GMBUS0 / 4] & 7u, slave = (v >> 1) & 0x7f;
    if (pin != gmbus_hdmi_pin || slave != 0x50) { gmbus2 = 1u << 10; return; }   /* NAK, bus idle */
    gmbus_pos = (v >> 8) & 0xff;
    gmbus_left = (v >> 16) & 0x1ff;
    gmbus2 = (1u << 9) | (1u << 11);
}

static uint32_t gmbus_data(void)
{
    assert(gmbus2 & (1u << 11));
    uint32_t w = 0;
    for (unsigned b = 0; b < 4 && gmbus_left; b++, gmbus_left--)
        w |= (uint32_t)hdmi_edid[gmbus_pos++ & 0xff] << (b * 8);
    gmbus2 = (1u << 9) | (gmbus_left ? (1u << 11) : (1u << 14));
    return w;
}

static uint32_t sim_read(void *ctx, uint32_t off)
{
    (void)ctx;
    assert(off < sizeof regs && !(off & 3));
    clock_us++;
    if (off == GMBUS2) return gmbus2;
    if (off == GMBUS3) return gmbus_data();
    if (off == 0x138124) return pcode_mailbox;
    return regs[off / 4];
}

static void sim_write(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    assert(off < sizeof regs && !(off & 3));
    clock_us++;
    for (unsigned ch = 0; ch < 4; ch++) {
        if (off == DP_AUX_CH_CTL(ch)) {
            if (v & CTL_BUSY) aux_exec(ch, v);
            else regs[off / 4] &= ~(v & (CTL_DONE | CTL_TIMEOUT | CTL_RECV));
            return;
        }
        if (off >= DP_AUX_CH_DATA(ch, 0) && off <= DP_AUX_CH_DATA(ch, 4)) { regs[off / 4] = v; return; }
    }
    if (off == GMBUS1) { gmbus_cmd(v); return; }
    if (off == GMBUS0 || off == 0xc5110) { regs[off / 4] = v; return; }
    if (off == 0x138124) {
        if ((v & (1u << 31)) && (v & 0xff) == 6) {
            regs[0x138128 / 4] = regs[0x138128 / 4] == 0 ? 0x0a080402u : 0x1e19140fu;
            pcode_mailbox = 0;
        }
        return;
    }
    if (off == 0x138128 || off == 0x13812c) { regs[off / 4] = v; return; }
    writes_outside_protocol++;             /* a capture must never touch a mode register */
}

static uint64_t sim_now(void *ctx) { (void)ctx; return clock_us += 3; }
static const struct intel_regs sim = { sim_read, sim_write, sim_now, 0 };

/* ---- simulated physical memory: OpRegion, raw VBT, GTT ------------------- */
#define OPREGION_PHYS 0x8d000000ull
#define BAR0 0xf0000000ull
#define BAR2 0xd0000000ull
#define STOLEN 0x7c000000ull
static uint8_t opregion[0x2000 + 0x2000];          /* OpRegion, then the RVDA VBT */
static uint64_t gtt_ptes[8192];
static unsigned maps, unmaps;
static uint32_t vbt_len;

static uint32_t cfg_read32(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    (void)ctx; (void)bus; (void)fn;
    if (dev == 2) {
        if (off == 0) return 0x9b418086u;
        if (off == 0xfc) return (uint32_t)OPREGION_PHYS;
        if (off == 0x5c) return (uint32_t)STOLEN | 1u;
        return 0x11110000u | off;
    }
    if (off == 0) return 0x02848086u;                   /* Comet Lake PCH-LP LPC */
    return 0x22220000u | off;
}

static const volatile uint8_t *sim_map(void *ctx, uint64_t phys, uint32_t len)
{
    (void)ctx;
    maps++;
    if (phys >= OPREGION_PHYS && phys + len <= OPREGION_PHYS + sizeof opregion)
        return opregion + (phys - OPREGION_PHYS);
    uint64_t gtt = BAR0 + 0x800000;
    if (phys >= gtt && phys + len <= gtt + sizeof gtt_ptes)
        return (const volatile uint8_t *)gtt_ptes + (phys - gtt);
    fprintf(stderr, "unexpected map %llx+%u\n", (unsigned long long)phys, len);
    abort();
}

static void sim_unmap(void *ctx, const volatile uint8_t *v, uint32_t len)
{
    (void)ctx; (void)v; (void)len;
    unmaps++;
}

/* ---- fixtures ------------------------------------------------------------- */
#include "intel_fixtures.h"

static void make_opregion(int rvda)
{
    memset(opregion, 0, sizeof opregion);
    memcpy(opregion, "IntelGraphicsMem", 16);
    put32(opregion + 0x10, 8);
    put32(opregion + 0x14, 2u << 24 | 1u << 16);     /* OpRegion 2.1 */
    put32(opregion + 0x58, 1u | 1u << 2);            /* ACPI and ASLE mailboxes */
    if (rvda) {
        vbt_len = make_vbt(opregion + 0x2000);
        put32(opregion + 0x3ba, 0x2000); put32(opregion + 0x3be, 0);   /* relative in 2.1 */
        put32(opregion + 0x3c2, vbt_len);
    } else {
        uint8_t tmp[0x2000];
        vbt_len = make_vbt(tmp);
        assert(vbt_len <= 0x1800);
        memcpy(opregion + 0x400, tmp, vbt_len);
    }
}

static void make_device(void)
{
    memset(regs, 0, sizeof regs);
    memset(dpcd, 0, sizeof dpcd);
    dpcd[0x000] = 0x12; dpcd[0x001] = 0x0a; dpcd[0x002] = 0x84; dpcd[0x003] = 0x01;
    dpcd[0x00e] = 0x00;
    put16(dpcd + 0x010, 8100); put16(dpcd + 0x012, 10800); put16(dpcd + 0x014, 13500);
    dpcd[0x070] = 0x01;
    dpcd[0x600] = 0x01;
    dpcd[0x700] = 0x03; dpcd[0x701] = 0x23;
    for (unsigned i = 0x100; i < 0x120; i++) dpcd[i] = (uint8_t)i;
    make_edid(edp_edid, "AUO", 0x40ed, 1920, 1080, 14250, "B156HAN02.1");
    make_edid(hdmi_edid, "DEL", 0xa0b1, 2560, 1440, 24150, "DELL U2717D");
    /* The firmware's mode, as the registers hold it. */
    regs[0x45404 / 4] = 0;                           /* power well 2 off */
    regs[0x46000 / 4] = (2u << 26) | 0x2a1;          /* CDCLK 337.5 MHz: (337500 - 1000) / 500 */
    regs[0x6f400 / 4] = (1u << 31) | (2u << 24) | (3u << 1);
    regs[0x64000 / 4] = (1u << 31) | (1u << 4);
    regs[0x70180 / 4] = (1u << 31) | (4u << 24);
    regs[0x6c058 / 4] = (1u << 0) | (1u << 1);       /* DPLL0: override, link rate 1350 = 2.7 Gbps */
    regs[0x7f008 / 4] = (1u << 31) | (1u << 30);     /* TRANS_CONF eDP: enabled, active */
    regs[0x6f000 / 4] = (2079u << 16) | 1919u;       /* H total 2080, active 1920 */
    regs[0x6f00c / 4] = (1109u << 16) | 1079u;       /* V total 1110, active 1080 */
    regs[0x6f040 / 4] = 142500u; regs[0x6f044 / 4] = 270000u;   /* link M/N: 142.5 MHz on 270 MHz */
    regs[0x6001c / 4] = (1919u << 16) | 1079u;       /* pipe A source */
    regs[0x70188 / 4] = 120;                         /* stride 7680 B */
    regs[0x70190 / 4] = (1079u << 16) | 1919u;
    regs[0x7019c / 4] = 0x10000;
    regs[0xc7200 / 4] = 1u << 31;
    for (unsigned i = 0; i < 8192; i++) gtt_ptes[i] = (STOLEN + 0x100000 + (uint64_t)i * 4096) | 1u;
    i2c_offset = 0; gmbus2 = 0; pcode_mailbox = 0; writes_outside_protocol = 0;
}

static struct intel_capture_env make_env(void)
{
    static const uint8_t boot_edid[128] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    struct intel_capture_env env;
    memset(&env, 0, sizeof env);
    env.regs = &sim;
    env.cfg_read32 = cfg_read32;
    env.map = sim_map;
    env.unmap = sim_unmap;
    env.functions[0] = (struct intel_capture_bdf){ 0, 2, 0, 0 };
    env.functions[1] = (struct intel_capture_bdf){ 0, 0x1f, 0, 1 };
    env.function_count = 2;
    env.bar0 = BAR0; env.bar0_size = 0x1000000;
    env.bar2 = BAR2; env.bar2_size = 0x10000000;
    env.pch_layout = INTEL_PCH_CNP;
    env.fb_addr = BAR2 + 0x10000; env.fb_width = 1920; env.fb_height = 1080;
    env.fb_pitch = 7680; env.fb_bpp = 32; env.fb_format = 1; env.firmware = 2;
    env.edid_size = 128; env.edid = boot_edid;
    return env;
}

/* ---- reading the capture back -------------------------------------------- */
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct seen { unsigned count[16]; unsigned dpcd_ok[4], dpcd_fail, dpcd_nack; int aux_edid_ok, gmbus_ok_pin, gmbus_nak; unsigned notes; int truncated_note, pw2_note; uint32_t pcode[2]; uint32_t mmio_6f400; };

static struct seen walk(const uint8_t *buf, size_t len)
{
    struct seen s;
    memset(&s, 0, sizeof s);
    s.gmbus_ok_pin = -1;
    assert(len >= 16 && !memcmp(buf, "EMBKDISP", 8) && get32(buf + 8) == INTEL_CAPTURE_VERSION);
    uint32_t sections = get32(buf + 12), n = 0;
    size_t off = 16;
    while (off < len) {
        assert(len - off >= 8);
        uint32_t tag = get32(buf + off), plen = get32(buf + off + 4);
        size_t padded = (plen + 3u) & ~(size_t)3u;
        assert(tag >= 1 && tag <= 12 && len - off - 8 >= padded);
        const uint8_t *p = buf + off + 8;
        s.count[tag]++;
        switch (tag) {
        case INTEL_CAP_MMIO: {
            uint32_t start = get32(p), count = get32(p + 4);
            assert(plen == 8 + count * 4);
            assert(!(start <= 0xc510c && 0xc510c < start + count * 4));   /* GMBUS3 never read */
            if (start <= 0x6f400 && 0x6f400 < start + count * 4) s.mmio_6f400 = get32(p + 8 + (0x6f400 - start));
            break;
        }
        case INTEL_CAP_DPCD: {
            int rc = (int)get32(p + 8);
            uint32_t addr = get32(p + 4), dlen = get32(p + 12);
            if (rc == 0) {
                assert(p[0] < 4);
                for (uint32_t i = 0; i < dlen; i++) assert(p[16 + i] == dpcd[addr + i]);
                s.dpcd_ok[p[0]]++;
            } else if (rc == -EMBK_EIO) s.dpcd_nack++;
            else s.dpcd_fail++;
            break;
        }
        case INTEL_CAP_AUXEDID:
            if ((int)get32(p + 4) == 0) { assert(!memcmp(p + 12, edp_edid, 256)); s.aux_edid_ok = 1; }
            break;
        case INTEL_CAP_GMBUSEDID:
            if ((int)get32(p + 4) == 0) { assert(!memcmp(p + 12, hdmi_edid, 256)); s.gmbus_ok_pin = p[0]; }
            else { assert((int)get32(p + 4) == -EMBK_ENXIO); s.gmbus_nak++; }
            break;
        case INTEL_CAP_PCODE:
            assert((int)get32(p + 8) == 0);
            s.pcode[get32(p + 4)] = get32(p + 12);
            break;
        case INTEL_CAP_RVDA:
            assert(get32(p + 8) == vbt_len && !memcmp(p + 12, opregion + 0x2000, vbt_len));
            break;
        case INTEL_CAP_OPREGION:
            assert(get32(p + 8) == 0x2000 && !memcmp(p + 12, opregion, 0x2000));
            break;
        case INTEL_CAP_GTT: {
            uint32_t first = get32(p + 8), pages = get32(p + 12), count = get32(p + 16);
            assert(first == 0x10 && pages == 2025 && count == 2025);
            for (uint32_t i = 0; i < count; i++)
                assert(get32(p + 20 + i * 8) == (uint32_t)gtt_ptes[first + i]);
            break;
        }
        case INTEL_CAP_NOTE:
            s.notes++;
            if (plen >= 9 && !memcmp(p, "capture t", 9)) s.truncated_note = 1;
            if (plen >= 9 && !memcmp(p, "AUX B-D n", 9)) s.pw2_note = 1;
            break;
        }
        off += 8 + padded;
        n++;
    }
    assert(off == len && n == sections);
    return s;
}

/* ---- the protocol units -------------------------------------------------- */
static void test_aux(void)
{
    make_device();
    uint8_t buf[256];
    defer_left = 3;                                       /* DEFERs are retried */
    assert(intel_dpcd_read(&sim, 0, 0, buf, 16) == 0 && !memcmp(buf, dpcd, 16));
    short_replies = 1;                                    /* short replies are reassembled */
    assert(intel_dpcd_read(&sim, 0, 0x100, buf, 32) == 0 && !memcmp(buf, dpcd + 0x100, 32));
    assert(intel_aux_i2c_read(&sim, 0, 0x50, 0, buf, 256) == 0 && !memcmp(buf, edp_edid, 256));
    short_replies = 0;
    assert(intel_dpcd_read(&sim, 0, 0x80, buf, 4) == -EMBK_EIO);         /* NACK */
    assert(intel_dpcd_read(&sim, 1, 0, buf, 4) == -EMBK_ETIMEDOUT);      /* empty port */
    assert(intel_aux_i2c_read(&sim, 0, 0x37, 0, buf, 1) == -EMBK_EIO);   /* nobody at 0x37 */
    defer_left = 1000;                                    /* a sink that only DEFERs */
    assert(intel_dpcd_read(&sim, 0, 0, buf, 1) == -EMBK_ETIMEDOUT);
    defer_left = 0;
    uint8_t w[3] = { 0x0a, 0x84, 0x01 };
    assert(intel_dpcd_write(&sim, 0, 0x100, w, 3) == 0 && !memcmp(dpcd + 0x100, w, 3));
    assert(intel_aux_transfer(&sim, 4, w, 3, buf, 20) == -EMBK_EINVAL);
    assert(intel_aux_transfer(&sim, 0, w, 21, buf, 20) == -EMBK_EINVAL);
    regs[DP_AUX_CH_CTL(2) / 4] = CTL_BUSY;                /* a controller stuck busy */
    sink_on[2] = 1;
    assert(intel_aux_transfer(&sim, 2, w, 3, buf, 20) == -EMBK_EBUSY);
    sink_on[2] = 0;
    assert(writes_outside_protocol == 0);
}

static void test_gmbus(void)
{
    make_device();
    uint8_t buf[256];
    assert(intel_gmbus_read(&sim, 1, 0x50, 0, buf, 256) == 0 && !memcmp(buf, hdmi_edid, 256));
    assert(gmbus2 == 0 && regs[GMBUS0 / 4] == 0);         /* stopped and released */
    assert(intel_gmbus_read(&sim, 1, 0x50, 0x80, buf, 16) == 0 && !memcmp(buf, hdmi_edid + 0x80, 16));
    assert(intel_gmbus_read(&sim, 2, 0x50, 0, buf, 128) == -EMBK_ENXIO);
    assert(gmbus2 == 0 && regs[GMBUS0 / 4] == 0);
    assert(intel_gmbus_read(&sim, 1, 0x50, 0, buf, 257) == -EMBK_EINVAL);
    assert(intel_gmbus_read(&sim, 0, 0x50, 0, buf, 1) == -EMBK_EINVAL);
    assert(writes_outside_protocol == 0);
}

static void test_capture(int rvda, int pw2, const char *out)
{
    make_device();
    make_opregion(rvda);
    if (pw2) { regs[0x45404 / 4] = 3u << 30; sink_on[2] = 1; }
    struct intel_capture_env env = make_env();
    static uint8_t buf[INTEL_CAPTURE_MAX];
    struct intel_capture_summary sum;
    unsigned before_maps = maps, before_unmaps = unmaps;
    size_t len = intel_capture_build(&env, buf, sizeof buf, &sum);
    assert(len > 16 && !sum.truncated);
    assert(maps - before_maps == unmaps - before_unmaps);    /* every mapping released */
    assert(writes_outside_protocol == 0);
    struct seen s = walk(buf, len);
    assert(s.count[INTEL_CAP_PCI] == 2 && s.count[INTEL_CAP_BARS] == 1 && s.count[INTEL_CAP_BOOTFB] == 1);
    assert(s.count[INTEL_CAP_OPREGION] == 1 && s.count[INTEL_CAP_RVDA] == (unsigned)rvda);
    assert(s.count[INTEL_CAP_MMIO] == 27 && s.mmio_6f400 == regs[0x6f400 / 4]);
    assert(s.aux_edid_ok && s.dpcd_nack == (pw2 ? 2u : 1u));
    assert(s.dpcd_ok[0] == 16 + 2 + 1 + 1 + 4 + 1 - 1);      /* every piece but the NACKed one */
    assert(s.dpcd_ok[2] == (pw2 ? s.dpcd_ok[0] : 0) && !s.dpcd_ok[1] && !s.dpcd_ok[3]);
    assert(s.gmbus_ok_pin == 1 && s.gmbus_nak == 3);
    assert(s.pcode[0] == 0x0a080402u && s.pcode[1] == 0x1e19140fu);
    assert(s.count[INTEL_CAP_GTT] == 1);
    assert(sum.opregion_version == 0x0201 && sum.vbt_bytes == vbt_len);
    assert(sum.dpcd_rev[0] == 0x12 && sum.aux_edid == (pw2 ? 5u : 1u) && sum.gmbus_edid == (1u << 1));
    if (pw2) {
        assert(sum.aux_bcd_powered && !s.pw2_note);
        assert(sum.dpcd_rev[2] == 0x12 && (sum.aux_edid & 4));   /* the Type-C sink on AUX C */
        assert(sum.dpcd_rev[1] == 0 && sum.dpcd_rev[3] == 0 && s.dpcd_fail == 2);
    } else {
        assert(!sum.aux_bcd_powered && s.pw2_note && s.dpcd_fail == 0);
    }
    if (out) {
        FILE *f = fopen(out, "wb");
        assert(f && fwrite(buf, 1, len, f) == len && fclose(f) == 0);
    }
    sink_on[2] = 0;

    /* Too small for everything: still a well-formed file that says so. */
    make_device();
    static uint8_t small[8192];
    len = intel_capture_build(&env, small, sizeof small, &sum);
    assert(len > 16 && sum.truncated);
    s = walk(small, len);
    assert(s.truncated_note);
    assert(intel_capture_build(&env, small, 32, &sum) == 0);
}

int main(void)
{
    test_aux();
    test_gmbus();
    test_capture(1, 0, 0);
    test_capture(0, 1, getenv("INTEL_CAPTURE_OUT"));
    printf("Intel display capture: AUX DEFER/NACK/short-reply/timeout/busy, I2C-over-AUX EDID, "
           "GMBUS indexed EDID read and NAK release, pcode latencies, OpRegion 2.1 RVDA and mailbox-4 VBT, "
           "GTT, power-well-2 gating, truncation, no mode-register writes OK (%u AUX messages)\n",
           aux_messages);
    return 0;
}
