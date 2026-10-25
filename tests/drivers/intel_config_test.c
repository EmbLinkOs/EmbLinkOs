/* intel_config_test.c -- the modeset arithmetic against values worked by hand
 * from the formulas (Intel PRM / i915 algorithms), not against the code. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/video/intel_config.h"
#include "display/edid_modes.h"

static int veto_216(uint32_t rate, void *ctx) { (void)ctx; return rate != 216000 && rate != 432000; }

static struct display_mode mode(uint32_t clk, unsigned w, unsigned h)
{
    const struct display_mode *k = edid_known_mode(w, h, w == 3840 && clk == 297000 ? 30 : 60);
    struct display_mode m;
    if (k) m = *k;
    else { memset(&m, 0, sizeof m); m.hdisplay = (uint16_t)w; m.vdisplay = (uint16_t)h; }
    m.clock_khz = clk;
    return m;
}

static void dp(void)
{
    uint8_t d[16] = { 0x12, 0x14, 0xc4, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct intel_dp_caps c;
    assert(intel_dp_caps_parse(d, 0, &c) == 0 && c.n_rates == 3 && c.rates[2] == 540000);
    assert(c.max_lanes == 4 && c.tps3 && c.enhanced_framing && c.downspread && !c.rate_table);
    struct intel_dp_source src = { 540000, 4, 0, 0 };
    struct intel_dp_link l;
    struct display_mode qhd = mode(241500, 2560, 1440);
    assert(intel_dp_pick_link(&c, &src, &qhd, 0, 0, 0, 0, &l) == 0);
    assert(l.rate == 270000 && l.lanes == 4 && l.bpp == 24 && l.bw_code == 10 && l.rate_select == 0xff);
    struct display_mode uhd60 = mode(594000, 3840, 2160);
    assert(intel_dp_pick_link(&c, &src, &uhd60, 0, 0, 0, 0, &l) == 0 && l.rate == 540000 && l.lanes == 4);
    src.max_rate = 270000;                      /* a source that stops at HBR */
    assert(intel_dp_pick_link(&c, &src, &uhd60, 0, 0, 0, 0, &l) == -1);
    struct display_mode uhd30 = mode(297000, 3840, 2160);
    assert(intel_dp_pick_link(&c, &src, &uhd30, 0, 0, 0, 0, &l) == 0 && l.rate == 270000 && l.lanes == 4 && l.bpp == 24);
    /* Fallback: lower the rate first, then halve the lanes; never below the mode. */
    src.max_rate = 540000;
    struct display_mode fhd = mode(148500, 1920, 1080);
    l = (struct intel_dp_link){ 540000, 4, 24, 20, 0xff };
    assert(intel_dp_link_fallback(&c, &src, &fhd, &l) == 0 && l.rate == 270000 && l.lanes == 4 && l.bw_code == 10);
    assert(intel_dp_link_fallback(&c, &src, &fhd, &l) == 0 && l.rate == 162000 && l.lanes == 4);
    assert(intel_dp_link_fallback(&c, &src, &fhd, &l) == 0 && l.rate == 540000 && l.lanes == 2);
    assert(intel_dp_link_fallback(&c, &src, &fhd, &l) == 0 && l.rate == 270000 && l.lanes == 2);
    /* 1.62 x2 cannot carry 1080p's 445500 kB/s, so the lanes halve: 5.4 x1 can. */
    assert(intel_dp_link_fallback(&c, &src, &fhd, &l) == 0 && l.rate == 540000 && l.lanes == 1);
    assert(intel_dp_link_fallback(&c, &src, &fhd, &l) == -1);   /* 2.7 x1 and below cannot */

    /* An eDP 1.4 panel with a rate table: set by index, highest rate, all lanes. */
    uint8_t rates[16] = { 0xa4, 0x1f, 0x30, 0x2a, 0xbc, 0x34 };   /* 8100, 10800, 13500 */
    uint8_t e[16] = { 0x14, 0x00, 0x84, 0x01 };
    assert(intel_dp_caps_parse(e, rates, &c) == 0 && c.rate_table && c.n_rates == 3);
    assert(c.rates[0] == 162000 && c.rates[1] == 216000 && c.rates[2] == 270000 && c.max_lanes == 4);
    struct display_mode panel = mode(142500, 1920, 1080);
    src = (struct intel_dp_source){ 540000, 4, veto_216, 0 };
    assert(intel_dp_pick_link(&c, &src, &panel, 1, 24, 0, 0, &l) == 0);
    assert(l.rate == 270000 && l.lanes == 4 && l.rate_select == 2 && l.bw_code == 0);
    assert(intel_dp_pick_link(&c, &src, &panel, 1, 24, 270000, 2, &l) == 0 && l.lanes == 2);   /* VBT's choice */
    assert(intel_dp_pick_link(&c, &src, &panel, 1, 24, 216000, 4, &l) == 0 && l.rate == 270000); /* PLL vetoes 2.16 */
    assert(intel_dp_pick_link(&c, &src, &panel, 1, 18, 0, 0, &l) == 0 && l.bpp == 18);          /* 6 bpc panel */
    /* A table that is not ascending is not trusted; the DPCD max rate stands. */
    uint8_t bad[16] = { 0xbc, 0x34, 0xa4, 0x1f };
    e[1] = 0x0a;
    assert(intel_dp_caps_parse(e, bad, &c) == 0 && c.rate_table && c.n_rates == 1);
    assert(intel_dp_caps_parse((uint8_t[16]){ 0 }, 0, &c) == -1);
}

static void m_n(void)
{
    struct intel_m_n mn;
    intel_link_m_n(24, 4, 148500, 270000, &mn);
    /* data: 24*148500 / (270000*4*8) of 0x8000000 = 55364812, then halved
     * together until both fit 24 bits: four times. */
    assert(mn.tu == 64 && mn.data_m == 3460300 && mn.data_n == 0x800000);
    assert(mn.link_m == 288358 && mn.link_n == 0x80000);           /* 148500/270000 of 0x80000 */
    intel_link_m_n(18, 2, 69300, 162000, &mn);
    assert(mn.data_n <= 0xffffff && mn.data_m <= 0xffffff && mn.link_n == 0x80000);
}

static void wrpll(void)
{
    struct intel_wrpll w;
    /* 148.5 MHz: AFE 742.5 MHz; only divider 12 lands near a centre -- 8910
     * MHz, 1 % under 9000. 12 = 2 x 3 x 2. DCO 8910/24 = 371 + 0x2000/0x8000. */
    assert(intel_wrpll_compute(148500, &w) == 0);
    assert(w.dco_khz == 8910000 && w.p0 == 2 && w.p1 == 3 && w.p2 == 2);
    assert(w.cfgcr1 == 0x80400173u && w.cfgcr2 == 0x3a5u);
    assert(intel_wrpll_clock(w.cfgcr1, w.cfgcr2) == 148500);
    assert(intel_wrpll_compute(74250, &w) == 0 && w.p0 == 2 && w.p1 == 6 && w.p2 == 2 && w.cfgcr2 == 0x6a5u);
    assert(intel_wrpll_compute(297000, &w) == 0 && w.p0 == 2 && w.p1 == 1 && w.p2 == 3);
    /* 27 MHz: 66 is the only even divider within the window (8910, 1 % under
     * 9000); 66 = 2 x 33 = 3 x 11 x 2. */
    assert(intel_wrpll_compute(27000, &w) == 0 && w.p0 == 3 && w.p1 == 11 && w.p2 == 2);
    /* 25.175 MHz: 76 x 125.875 = 9566.5 MHz, 0.35 % under 9600, beats every
     * other candidate; 76 = 2 x 19 x 2. */
    assert(intel_wrpll_compute(25175, &w) == 0 && w.p0 == 2 && w.p1 == 19 && w.p2 == 2 && w.dco_khz == 9566500);
    uint32_t back = intel_wrpll_clock(w.cfgcr1, w.cfgcr2);
    assert(back + 2 >= 25175 && back <= 25175);
    /* Every clock in the HDMI range gets a PLL that makes it. */
    for (uint32_t clk = 25000; clk <= 300000; clk += 125) {
        assert(intel_wrpll_compute(clk, &w) == 0);
        uint32_t got = intel_wrpll_clock(w.cfgcr1, w.cfgcr2);
        assert(got + clk / 2000 + 2 >= clk && got <= clk + clk / 2000 + 2);
        uint64_t dco = w.dco_khz;
        assert(dco >= 7896000 && dco <= 9696000);
    }
    assert(intel_wrpll_compute(0, &w) == -1);
}

static void cdclk(void)
{
    assert(intel_cdclk_pick(142500, 8100000) == 337500 && intel_cdclk_pick(400000, 8100000) == 450000);
    assert(intel_cdclk_pick(594000, 8100000) == 675000 && intel_cdclk_pick(700000, 8100000) == 0);
    assert(intel_cdclk_pick(300000, 8640000) == 308571 && intel_cdclk_pick(1, 1) == 0);
    assert(intel_cdclk_ctl(337500) == ((2u << 26) | 673) && intel_cdclk_ctl(675000) == ((3u << 26) | 1348));
    assert(intel_cdclk_ctl(308571) == ((2u << 26) | 615) && intel_cdclk_ctl(432000) == 862);
    assert(intel_cdclk_voltage_level(337500) == 0 && intel_cdclk_voltage_level(450000) == 1 &&
           intel_cdclk_voltage_level(540000) == 2 && intel_cdclk_voltage_level(675000) == 3);
    assert(intel_dpll_link_rate_code(270000) == 1 && intel_dpll_link_rate_code(540000) == 0 &&
           intel_dpll_link_rate_code(216000) == 4 && intel_dpll_link_rate_code(123) == -1);
}

static void ddb_wm(void)
{
    struct intel_ddb p[2], c[2];
    unsigned one[1] = { 1920 };
    intel_ddb_split(1, one, p, c);
    assert(p[0].start == 0 && p[0].end == 860 && c[0].start == 860 && c[0].end == 892);
    unsigned two[2] = { 1920, 2560 };
    intel_ddb_split(2, two, p, c);
    assert(p[0].start == 0 && p[0].end == 374 && c[0].start == 374 && c[0].end == 382);
    assert(p[1].start == 382 && p[1].end == 884 && c[1].start == 884 && c[1].end == 892);

    uint16_t lat[8];
    intel_wm_latency_adjust(0x0a080402u, 0x1e19140fu, lat);
    static const uint16_t want_lat[8] = { 3, 4, 8, 10, 15, 20, 25, 30 };
    assert(!memcmp(lat, want_lat, sizeof lat));
    intel_wm_latency_adjust(0x0a080400u, 0x00000000u, lat);       /* level 0 reads 0: +2 for read latency */
    static const uint16_t want_zero[8] = { 3, 6, 10, 12, 0, 0, 0, 0 };
    assert(!memcmp(lat, want_zero, sizeof lat));
    intel_wm_latency_adjust(0x0a000402u, 0x1e19140fu, lat);       /* a 0 at level 2 ends it there */
    assert(lat[1] == 4 && lat[2] == 0 && lat[7] == 0);

    /* 1080p60 (148.5 MHz, htotal 2200), linear XRGB: 16 blocks a line,
     * linetime 15 us; worked level by level from skl_compute_plane_wm. */
    intel_wm_latency_adjust(0x0a080402u, 0x1e19140fu, lat);
    struct intel_wm_level wm[8];
    assert(intel_plane_wm(lat, 148500, 2200, 1920, 4, 860, wm) == 7);
    static const uint16_t blocks[8] = { 5, 7, 12, 14, 20, 26, 32, 37 };
    static const uint8_t lines[8] = { 0, 1, 1, 1, 2, 2, 2, 3 };
    for (unsigned i = 0; i < 8; i++) assert(wm[i].enabled && wm[i].blocks == blocks[i] && wm[i].lines == lines[i]);
    assert(intel_plane_wm(lat, 148500, 2200, 1920, 4, 20, wm) == 3 && !wm[4].enabled && !wm[7].enabled);
    assert(intel_plane_wm(lat, 148500, 2200, 1920, 4, 5, wm) == -1);
}

static void pfit(void)
{
    struct intel_pfit f;
    assert(intel_pfit_compute(1280, 720, 1920, 1080, &f) == 0 && f.x == 0 && f.y == 0 && f.w == 1920 && f.h == 1080);
    /* scale 1280/1920 in 16.16 = 43690: phase -0x8000 + 21845 -> 54613 -> 13652. */
    assert(f.hphase == 13652 && f.vphase == 13652);
    assert(intel_pfit_compute(1024, 768, 1920, 1080, &f) == 0 && f.w == 1440 && f.h == 1080 && f.x == 240 && f.y == 0);
    assert(intel_pfit_compute(1920, 1200, 1920, 1080, &f) == 0 && f.w == 1728 && f.x == 96);
    assert(intel_pfit_compute(1366, 768, 1920, 1080, &f) == 0 && f.y == 0 && f.h == 1080);
    assert(intel_pfit_compute(3840, 2160, 1920, 1080, &f) == 0);
    assert(intel_pfit_compute(4000, 2160, 1920, 1080, &f) == -1 && intel_pfit_compute(4, 4, 1920, 1080, &f) == -1);
}

int main(void)
{
    dp();
    m_n();
    wrpll();
    cdclk();
    ddb_wm();
    pfit();
    puts("Intel modeset arithmetic: DP caps/eDP rate tables/link choice/fallback, M/N, HDMI WRPLL across "
         "25-300 MHz, CDCLK, DDB split, watermark latencies and levels, panel fitter OK");
    return 0;
}
