/* intel_hwsim.h -- a register model of a Gen9 display and its eDP/DP sink,
 * shared by the Intel display host tests. Power wells follow their requests,
 * PLLs lock only when programmed, DDI buffers report idle, transcoders have
 * state bits, the panel power sequencer answers, pcode handles CDCLK
 * requests, frame counters move, and the sink demands `need_swing` and
 * `need_preemph` before it calls the link trained. Every write is traced. */
#ifndef EMBLINK_TEST_INTEL_HWSIM_H
#define EMBLINK_TEST_INTEL_HWSIM_H
#include <assert.h>
#include <string.h>
#include "drivers/video/intel_regs.h"
#include "drivers/video/intel_dpaux.h"

static uint32_t regs[0x200000 / 4];
static uint64_t clock_us;
struct write { uint32_t off, val; };
static struct write trace[20000];
static unsigned ntrace;

/* the sink */
static uint8_t dpcd[0x3000];
static unsigned need_swing = 1, need_preemph = 1, sink_max_rate = 540000;
static unsigned sink_dead;          /* answers, never trains */
static unsigned sink_gone;          /* does not answer AUX reads at all */
static unsigned rate_now;           /* link symbol clock the sink was configured to, kHz */
static const uint8_t *sink_edid;   /* the panel's EDID over I2C-over-AUX, or NULL */
static unsigned i2c_at;

static unsigned lanes_now(void) { return dpcd[0x101] & 0x1f; }

static void sink_status(void)
{
    unsigned pat = dpcd[0x102] & 3u, lanes = lanes_now();
    unsigned v = dpcd[0x103] & 3u, p = (dpcd[0x103] >> 3) & 3u;
    int rate_ok = rate_now && rate_now <= sink_max_rate && !sink_dead;
    uint8_t lane = 0, req = 0;
    if (pat == 1) {
        if (rate_ok && v >= need_swing) lane = 0x1;
        req = (uint8_t)(need_swing | (0u << 2));
    } else if (pat == 2 || pat == 3) {
        lane = rate_ok && v >= need_swing ? 0x1 : 0;
        if (lane && p >= need_preemph) lane = 0x7;
        req = (uint8_t)(need_swing | (need_preemph << 2));
    }
    memset(dpcd + 0x202, 0, 6);
    for (unsigned l = 0; l < lanes; l++) {
        dpcd[0x202 + l / 2] |= (uint8_t)(lane << ((l & 1) * 4));
        dpcd[0x206 + l / 2] |= (uint8_t)(req << ((l & 1) * 4));
    }
    if (lane == 0x7) dpcd[0x204] = 0x1;
}

/* ---- external monitors ------------------------------------------------------
 * Off unless a test sets ext_mode: AUX B-D then answer from ext[] rather than
 * the panel's sink (a missing sink never answers), GMBUS serves an HDMI
 * monitor's EDID on gmbus_sim_pin, and the test drives SDEISR's live bits.
 * External DP sinks train at level 0; `dead` ones never do. */
static int ext_mode;
struct ext_sink { int present, dead; uint8_t dpcd[0x800]; const uint8_t *edid; unsigned i2c_at; };
static struct ext_sink ext[4];
static const uint8_t *gmbus_edid;
static unsigned gmbus_sim_pin;
static uint32_t gmbus2, gmbus_left;
static unsigned gmbus_pos;

static void ext_status(struct ext_sink *e)
{
    unsigned lanes = e->dpcd[0x101] & 0x1f, pat = e->dpcd[0x102] & 3u;
    uint8_t lane = e->dead ? 0 : pat == 1 ? 0x1 : (pat == 2 || pat == 3) ? 0x7 : 0;
    memset(e->dpcd + 0x202, 0, 6);
    for (unsigned l = 0; l < lanes; l++) e->dpcd[0x202 + l / 2] |= (uint8_t)(lane << ((l & 1) * 4));
    if (lane == 0x7) e->dpcd[0x204] = 0x1;
}

static void ext_aux(unsigned ch, uint32_t ctl)
{
    struct ext_sink *e = &ext[ch];
    if (!e->present) { regs[DP_AUX_CH_CTL(ch) / 4] = 1u << 28; return; }   /* nothing there: a timeout */
    unsigned size = (ctl >> 20) & 0x1f;
    uint8_t tx[20];
    for (unsigned i = 0; i < size; i++)
        tx[i] = (uint8_t)(regs[DP_AUX_CH_DATA(ch, i / 4) / 4] >> ((3 - i % 4) * 8));
    unsigned req = tx[0] >> 4, addr = (unsigned)(tx[0] & 0xf) << 16 | (unsigned)tx[1] << 8 | tx[2];
    uint8_t reply[20] = { 0 };
    unsigned n = 1;
    if (req == 0x8) {
        unsigned len = tx[3] + 1u;
        for (unsigned i = 0; i < len && addr + i < sizeof e->dpcd; i++) e->dpcd[addr + i] = tx[4 + i];
        if (addr <= 0x102 && addr + len > 0x102) ext_status(e);
    } else if (req == 0x9) {
        unsigned len = tx[3] + 1u;
        if (addr == 0x202) ext_status(e);
        for (unsigned i = 0; i < len; i++) reply[1 + i] = addr + i < sizeof e->dpcd ? e->dpcd[addr + i] : 0;
        n += len;
    } else {
        unsigned rd = req & 1u, mot = req & 4u;
        if (addr != 0x50 || !e->edid) reply[0] = 0x40;
        else if (size == 3) { if (!mot) e->i2c_at = 0; }
        else if (!rd) e->i2c_at = tx[4];
        else {
            unsigned len = tx[3] + 1u;
            for (unsigned i = 0; i < len; i++) reply[1 + i] = e->edid[(e->i2c_at + i) & 0xff];
            e->i2c_at += len;
            n += len;
        }
    }
    for (unsigned i = 0; i < 5; i++) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; b++) if (i * 4 + b < n) w |= (uint32_t)reply[i * 4 + b] << ((3 - b) * 8);
        regs[DP_AUX_CH_DATA(ch, i) / 4] = w;
    }
    regs[DP_AUX_CH_CTL(ch) / 4] = (1u << 30) | (n << 20);
}

/* GMBUS: indexed reads at slave 0x50 on one pin; anything else NAKs. */
static void gmbus_cmd(uint32_t v)
{
    if (v & (1u << 31)) { gmbus2 = 0; return; }                  /* SW_CLR_INT */
    if (!(v & (1u << 30))) return;                                /* not SW_RDY */
    unsigned cycle = (v >> 25) & 7u;
    if (cycle == 4) { gmbus2 = 0; return; }                       /* STOP */
    unsigned pin = regs[0xc5100 / 4] & 7u, slave = (v >> 1) & 0x7f;
    if (!gmbus_edid || pin != gmbus_sim_pin || slave != 0x50) { gmbus2 = 1u << 10; return; }
    gmbus_pos = (v >> 8) & 0xff;
    gmbus_left = (v >> 16) & 0x1ff;
    gmbus2 = (1u << 9) | (1u << 11);
}

static uint32_t gmbus_data(void)
{
    uint32_t w = 0;
    for (unsigned b = 0; b < 4 && gmbus_left; b++, gmbus_left--) w |= (uint32_t)gmbus_edid[gmbus_pos++ & 0xff] << (b * 8);
    gmbus2 = (1u << 9) | (gmbus_left ? (1u << 11) : (1u << 14));
    return w;
}

static void aux_exec(unsigned ch, uint32_t ctl)
{
    if (ext_mode && ch != 0) { ext_aux(ch, ctl); return; }
    unsigned size = (ctl >> 20) & 0x1f;
    uint8_t tx[20];
    for (unsigned i = 0; i < size; i++)
        tx[i] = (uint8_t)(regs[DP_AUX_CH_DATA(ch, i / 4) / 4] >> ((3 - i % 4) * 8));
    unsigned req = tx[0] >> 4, addr = (unsigned)(tx[0] & 0xf) << 16 | (unsigned)tx[1] << 8 | tx[2];
    uint8_t reply[20] = { 0 };
    unsigned n = 1;
    if (req == 0x8) {
        unsigned len = tx[3] + 1u;
        for (unsigned i = 0; i < len; i++) dpcd[addr + i] = tx[4 + i];
        if (addr <= 0x100 && addr + len > 0x100 && dpcd[0x100]) rate_now = dpcd[0x100] * 27000u;
        if (addr <= 0x115 && addr + len > 0x115) {
            static const unsigned table[3] = { 162000, 216000, 270000 };
            rate_now = table[dpcd[0x115] % 3];
        }
        if (addr <= 0x103 && addr + len > 0x102) sink_status();
    } else if (req == 0x9) {
        unsigned len = tx[3] + 1u;
        if (addr == 0x202) sink_status();
        if (sink_gone) { regs[DP_AUX_CH_CTL(ch) / 4] = 1u << 28; return; }   /* unplugged: no answer at all */
        for (unsigned i = 0; i < len; i++) reply[1 + i] = dpcd[addr + i];
        n += len;
    } else {                                      /* I2C over AUX: the panel's EDID at 0x50 */
        unsigned rd = req & 1u, mot = req & 4u;
        if (addr != 0x50 || !sink_edid) reply[0] = 0x40;
        else if (size == 3) { if (!mot) i2c_at = 0; }
        else if (!rd) i2c_at = tx[4];
        else {
            unsigned len = tx[3] + 1u;
            for (unsigned i = 0; i < len; i++) reply[1 + i] = sink_edid[(i2c_at + i) & 0xff];
            i2c_at += len;
            n += len;
        }
    }
    for (unsigned i = 0; i < 5; i++) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; b++) if (i * 4 + b < n) w |= (uint32_t)reply[i * 4 + b] << ((3 - b) * 8);
        regs[DP_AUX_CH_DATA(ch, i) / 4] = w;
    }
    regs[DP_AUX_CH_CTL(ch) / 4] = (1u << 30) | (n << 20);
}

static uint32_t pcode_reply;
static unsigned cdclk_prepares, cdclk_levels[4], nlevels;

static uint32_t sim_read(void *ctx, uint32_t off)
{
    (void)ctx;
    assert(off < sizeof regs && !(off & 3));
    clock_us++;
    if (off >= 0x70040 && off < 0x73000 && (off & 0xfff) == 0x040) regs[off / 4]++;   /* frame counter */
    if (ext_mode && off == 0xc5108) return gmbus2;
    if (ext_mode && off == 0xc510c) return gmbus_data();
    return regs[off / 4];
}

static void sim_write(void *ctx, uint32_t off, uint32_t v)
{
    (void)ctx;
    assert(off < sizeof regs && !(off & 3));
    clock_us++;
    if (ntrace < sizeof trace / sizeof trace[0]) trace[ntrace++] = (struct write){ off, v };
    if (ext_mode && off == 0xc5104) { gmbus_cmd(v); return; }
    for (unsigned ch = 0; ch < 4; ch++)
        if (off == DP_AUX_CH_CTL(ch)) {
            if (v & (1u << 31)) aux_exec(ch, v);
            else regs[off / 4] &= ~(v & ((1u << 30) | (1u << 28) | (1u << 25)));
            return;
        }
    regs[off / 4] = v;
    if (off == 0x45404) {                         /* power wells follow their requests */
        uint32_t state = 0;
        for (unsigned i = 0; i < 16; i++) if (v & (2u << (2 * i))) state |= 1u << (2 * i);
        regs[off / 4] = v | state;
        if (state & (1u << 30)) regs[0x42000 / 4] |= 1u << 25;
    }
    static const uint32_t pllctl[4] = { 0x46010, 0x46014, 0x46040, 0x46060 };
    for (unsigned id = 1; id < 4; id++)
        if (off == pllctl[id]) {
            uint32_t ctrl1 = regs[0x6c058 / 4] >> (id * 6);
            int hdmi = (ctrl1 >> 5) & 1, programmed = hdmi ? (regs[(0x6c040 + (id - 1) * 8) / 4] >> 31) : (ctrl1 & 1);
            if ((v >> 31) && programmed) regs[0x6c060 / 4] |= 1u << (id * 8);
            else regs[0x6c060 / 4] &= ~(1u << (id * 8));
        }
    if (off >= 0x64000 && off < 0x64500 && (off & 0xff) == 0) {           /* DDI_BUF_CTL */
        if (v >> 31) regs[off / 4] &= ~(1u << 7); else regs[off / 4] |= 1u << 7;
    }
    if (off >= 0x64040 && off < 0x64500 && (off & 0xff) == 0x40) {        /* DP_TP_CTL */
        if (((v >> 8) & 7) == 2) regs[(off + 4) / 4] |= 1u << 25; else regs[(off + 4) / 4] &= ~(1u << 25);
    }
    if (off == 0x70008 || off == 0x71008 || off == 0x72008 || off == 0x7f008) {   /* TRANS_CONF */
        if (v >> 31) regs[off / 4] |= 1u << 30; else regs[off / 4] &= ~(1u << 30);
    }
    if (off == 0xc7204) {                         /* panel power */
        regs[0xc7200 / 4] = (v & 1) ? (1u << 31) | 0x8 : 0;
    }
    if (off == 0x138124 && (v >> 31)) {           /* pcode */
        if ((v & 0xff) == 0x6)                    /* memory latencies: levels 0-3, then 4-7 */
            regs[0x138128 / 4] = regs[0x138128 / 4] == 0 ? 0x0a080402u : 0x1e19140fu;
        if ((v & 0xff) == 0x7) {
            uint32_t d = regs[0x138128 / 4];
            if (d == 0x3) { cdclk_prepares++; regs[0x138128 / 4] = pcode_reply; }
            else if (nlevels < 4) cdclk_levels[nlevels++] = d;
        }
        regs[off / 4] = 0;
    }
    if (off >= 0x44408 && off < 0x44440 && (off & 0xf) == 8) regs[off / 4] &= ~v;   /* IIR: write 1 to clear */
}

static uint64_t sim_now(void *ctx) { (void)ctx; return clock_us += 7; }
static const struct intel_regs sim = { sim_read, sim_write, sim_now, 0 };

#endif
