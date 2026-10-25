/* intel_hw_test.c -- the Gen9 display programming against a register model:
 * power wells that follow their requests, PLLs that lock only when
 * programmed, DDI buffers with idle status, transcoders with state bits, a
 * panel power sequencer, pcode, a frame counter, and a DP sink that demands a
 * particular voltage swing and pre-emphasis before it calls the link trained.
 * Every write is traced, so the test checks ordering as well as values. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/video/intel_hw.h"
#include "drivers/video/intel_dpaux.h"
#include "display/edid_modes.h"
#include "include/errno.h"

#include "intel_hwsim.h"

static int first_write(uint32_t off, uint32_t mask, uint32_t want)
{
    for (unsigned i = 0; i < ntrace; i++) if (trace[i].off == off && (trace[i].val & mask) == want) return (int)i;
    return -1;
}

static int last_write(uint32_t off)
{
    for (int i = (int)ntrace - 1; i >= 0; i--) if (trace[i].off == off) return i;
    return -1;
}

static void reset(void)
{
    memset(regs, 0, sizeof regs);
    memset(dpcd, 0, sizeof dpcd);
    ntrace = 0;
    need_swing = 1; need_preemph = 1; sink_max_rate = 540000; sink_dead = 0; rate_now = 0;
    pcode_reply = 1; cdclk_prepares = 0; nlevels = 0;
    regs[0x46000 / 4] = (2u << 26) | 0x2a1;           /* CDCLK 337.5 */
    regs[0x6c058 / 4] = 1u | (1u << 1);               /* DPLL0: override, 1350 */
    regs[0x45404 / 4] = (1u << 29) | (1u << 28);      /* PW1 */
    for (unsigned d = 0; d < 5; d++) regs[(0x64000 + d * 0x100) / 4] = 1u << 7;   /* idle */
}

static struct intel_crtc_cfg edp_cfg(void)
{
    struct intel_crtc_cfg c;
    memset(&c, 0, sizeof c);
    c.pipe = INTEL_PIPE_A; c.transcoder = INTEL_TRANS_EDP; c.ddi = 0; c.kind = INTEL_OUT_EDP; c.aux_ch = 0; c.dpll = 0;
    c.timing = (struct display_mode){ 142500, 1920, 1968, 2000, 2080, 1080, 1083, 1088, 1110, DISPLAY_MODE_HSYNC_POSITIVE, 0 };
    c.src_w = 1920; c.src_h = 1080; c.bpc = 8;
    c.link = (struct intel_dp_link){ 270000, 4, 24, 0, 2 };
    c.enhanced_framing = 1; c.tps3 = 1; c.dpcd_rev = 0x14;
    c.surf_ggtt = 0x00400000; c.stride = 7680;
    c.plane_ddb = (struct intel_ddb){ 0, 860 }; c.cursor_ddb = (struct intel_ddb){ 860, 892 };
    uint16_t lat[8];
    intel_wm_latency_adjust(0x0a080402u, 0x1e19140fu, lat);
    intel_plane_wm(lat, 142500, 2080, 1920, 4, 860, c.wm);
    return c;
}

static void test_edp(void)
{
    reset();
    struct intel_hw hw;
    struct intel_vbt vbt;
    memset(&vbt, 0, sizeof vbt);
    vbt.pps.valid = 1; vbt.pps.t1_t3 = 2000; vbt.pps.t8 = 500; vbt.pps.t9 = 500; vbt.pps.t10 = 500; vbt.pps.t11_t12 = 5000;
    vbt.backlight.valid = 1; vbt.backlight.pwm = 1; vbt.backlight.pwm_hz = 200;
    intel_hw_init(&hw, &sim, INTEL_PCH_CNP, &vbt);
    assert(hw.cdclk_khz == 337500 && hw.vco_khz == 8100000);
    assert(hw.pps.t1_t3 == 2000 && regs[0xc7208 / 4] == ((2000u << 16) | 500));   /* zero registers: the VBT's */
    assert(((regs[0xc7204 / 4] >> 4) & 0x1f) == 5);                                 /* 500 ms cycle */
    assert(hw.pwm_freq == 96000);                     /* 19.2 MHz / 200 Hz */

    struct intel_crtc_cfg c = edp_cfg();
    ntrace = 0;
    int rc = intel_hw_enable(&hw, &c, 0, 0);
    if (rc) fprintf(stderr, "eDP: %s\n", hw.last_error);
    assert(rc == 0);
    /* values */
    assert(regs[0x6f000 / 4] == ((2079u << 16) | 1919) && regs[0x6f00c / 4] == ((1109u << 16) | 1079));
    assert(regs[0x6f008 / 4] == ((1999u << 16) | 1967) && regs[0x6f014 / 4] == ((1087u << 16) | 1082));
    assert(regs[0x6001c / 4] == ((1919u << 16) | 1079));
    struct intel_m_n mn;
    intel_link_m_n(24, 4, 142500, 270000, &mn);
    assert(regs[0x6f030 / 4] == ((63u << 25) | mn.data_m) && regs[0x6f034 / 4] == mn.data_n);
    assert(regs[0x6f040 / 4] == mn.link_m && regs[0x6f044 / 4] == mn.link_n);
    assert(regs[0x6f410 / 4] == ((1u << 5) | 1u));
    assert(regs[0x6f400 / 4] == ((1u << 31) | (2u << 24) | (1u << 16) | (3u << 1)));   /* DP SST, H+, x4, eDP A on */
    assert(regs[0x7f008 / 4] & (1u << 30));
    assert(regs[0x70180 / 4] == ((1u << 31) | (4u << 24)) && regs[0x70188 / 4] == 120);
    assert(regs[0x70190 / 4] == ((1079u << 16) | 1919) && regs[0x7019c / 4] == 0x00400000);
    assert(regs[0x7027c / 4] == ((859u << 16) | 0) && regs[0x7017c / 4] == ((891u << 16) | 860));
    assert(regs[0x70240 / 4] == ((1u << 31) | c.wm[0].blocks) && (regs[0x7025c / 4] >> 31));
    assert(regs[0x45270 / 4] == (2080u * 8000u + 142499u) / 142500u);
    assert((regs[0xc7204 / 4] & 5u) == 5u && (regs[0xc8250 / 4] >> 31));            /* power + backlight */
    assert(regs[0xc8254 / 4] == 96000 && regs[0xc8258 / 4] == 96000);
    assert(((regs[0x64000 / 4] >> 24) & 0xf) == 5 && (regs[0x64000 / 4] >> 31) && (regs[0x64000 / 4] & (1u << 4)));   /* v1 p1, A x4 */
    assert(((regs[0x64040 / 4] >> 8) & 7) == 3 && (regs[0x64040 / 4] & (1u << 18)));  /* normal, enhanced framing */
    assert(dpcd[0x100] == 0 && dpcd[0x115] == 2 && dpcd[0x101] == (4 | 0x80) && dpcd[0x108] == 1);
    assert(dpcd[0x102] == 0 && dpcd[0x600] == 1 && dpcd[0x103] == (1 | (1 << 3)));
    /* ordering */
    int pll = last_write(0x6c058), clk = first_write(0x6c05c, 0, 0), panel = first_write(0xc7204, 1, 1);
    int tp = first_write(0x64040, 1u << 31, 1u << 31), buf = first_write(0x64000, 1u << 31, 1u << 31);
    int func = first_write(0x6f400, 1u << 31, 1u << 31), conf = first_write(0x7f008, 1u << 31, 1u << 31);
    int surf = last_write(0x7019c), blc = first_write(0xc7204, 4, 4), normal = first_write(0x64040, 7u << 8, 3u << 8);
    assert(pll >= 0 && pll < clk && clk < panel && panel < tp && tp < buf && buf < normal);
    assert(normal < func && func < conf && conf < surf && surf < blc);
    assert(last_write(0x7027c) < conf && last_write(0x70240) < conf);               /* DDB and WMs before the pipe runs */

    /* Disable, keeping the panel for a mode change that follows. */
    ntrace = 0;
    intel_hw_disable(&hw, &c, 1);
    assert(!(regs[0x70180 / 4] >> 31) && !(regs[0x7f008 / 4] & (1u << 30)) && !(regs[0x6f400 / 4] >> 31));
    assert(!(regs[0x64000 / 4] >> 31) && (regs[0x64000 / 4] & (1u << 7)));
    assert((regs[0xc7204 / 4] & 1) && !(regs[0xc7204 / 4] & 4) && !(regs[0xc8250 / 4] >> 31));
    assert(first_write(0xc7204, 4, 0) < first_write(0x70180, 0, 0));                   /* backlight off first */
    assert(first_write(0x7f008, 1u << 31, 0) < first_write(0x64000, 1u << 31, 0));     /* pipe before the DDI */
    assert(regs[0x6c05c / 4] & (1u << 15));                                            /* DDI A clock off */
    intel_hw_disable(&hw, &c, 0);
    assert(!(regs[0xc7204 / 4] & 1) && !(regs[0xc7200 / 4] >> 31));
}

static void test_dp_and_fallback(void)
{
    reset();
    struct intel_hw hw;
    intel_hw_init(&hw, &sim, INTEL_PCH_CNP, 0);
    uint8_t d[16] = { 0x12, 0x14, 0xc4, 0x01 };
    struct intel_dp_caps caps;
    assert(intel_dp_caps_parse(d, 0, &caps) == 0);
    struct intel_dp_source src = { 540000, 4, 0, 0 };
    struct intel_crtc_cfg c;
    memset(&c, 0, sizeof c);
    c.pipe = INTEL_PIPE_B; c.transcoder = 1; c.ddi = 2; c.kind = INTEL_OUT_DP; c.aux_ch = 2; c.dpll = 1;
    c.timing = *edid_known_mode(2560, 1440, 60);
    c.src_w = 2560; c.src_h = 1440; c.bpc = 8; c.dpcd_rev = 0x12; c.tps3 = 1;
    c.link = (struct intel_dp_link){ 540000, 4, 24, 20, 0xff };                        /* the sink only manages 2.7 */
    c.surf_ggtt = 0x01000000; c.stride = 10240;
    c.plane_ddb = (struct intel_ddb){ 0, 860 }; c.cursor_ddb = (struct intel_ddb){ 860, 892 };
    c.wm[0] = (struct intel_wm_level){ 1, 0, 8 };
    sink_max_rate = 270000;
    hw.cdclk_khz = 337500;
    int rc = intel_hw_enable(&hw, &c, &caps, &src);
    if (rc) fprintf(stderr, "DP: %s\n", hw.last_error);
    assert(rc == 0 && c.link.rate == 270000 && c.link.lanes == 4 && dpcd[0x100] == 10);
    assert((regs[0x45404 / 4] & (1u << 31)) && (regs[0x45404 / 4] & (2u << 6)));     /* PW2, DDI C IO */
    assert((regs[0x46014 / 4] >> 31) && (regs[0x6c060 / 4] & (1u << 8)));             /* DPLL1 on and locked */
    assert(((regs[0x6c058 / 4] >> 6) & 0x3f) == (1u | (1u << 1)));                    /* DPLL1: override, 1350 */
    uint32_t c2 = regs[0x6c05c / 4];
    assert(!(c2 & (1u << 17)) && ((c2 >> 7) & 3) == 1 && (c2 & (1u << 6)));           /* DDI C <- DPLL1 */
    assert(((regs[0x61400 / 4] >> 28) & 7) == 2 && (regs[0x71008 / 4] & (1u << 30)));
    assert(regs[0x64240 / 4] & (1u << 31));
    struct intel_m_n mn;
    intel_link_m_n(24, 4, 241500, 270000, &mn);
    assert(regs[0x61040 / 4] == mn.link_m);                                            /* M/N for the link that trained */

    /* A sink that never trains: everything turned on is turned off again. */
    intel_hw_disable(&hw, &c, 0);
    assert(!(regs[0x46014 / 4] >> 31));                                                /* last user gone: DPLL1 off */
    sink_dead = 1;
    c.link = (struct intel_dp_link){ 270000, 4, 24, 10, 0xff };
    rc = intel_hw_enable(&hw, &c, &caps, &src);
    assert(rc == -EMBK_EIO && !strcmp(hw.last_error, "DP: clock recovery failed"));
    assert(!(regs[0x64200 / 4] >> 31) && (regs[0x6c05c / 4] & (1u << 17)) && !(regs[0x46014 / 4] >> 31));
    assert(!(regs[0x71008 / 4] >> 31) && hw.dpll_users[1] == 0);
}

static void test_hdmi(void)
{
    reset();
    struct intel_hw hw;
    intel_hw_init(&hw, &sim, INTEL_PCH_CNP, 0);
    struct intel_crtc_cfg c;
    memset(&c, 0, sizeof c);
    c.pipe = INTEL_PIPE_B; c.transcoder = 1; c.ddi = 1; c.kind = INTEL_OUT_HDMI; c.aux_ch = INTEL_NO_AUX; c.dpll = 2;
    c.timing = *edid_known_mode(1920, 1080, 60);
    c.timing.vic = 16;
    c.src_w = 1920; c.src_h = 1080; c.bpc = 8; c.hdmi_sink = 1; c.hdmi_level = 0xff;
    assert(intel_wrpll_compute(148500, &c.wrpll) == 0);
    c.surf_ggtt = 0x01000000; c.stride = 7680;
    c.plane_ddb = (struct intel_ddb){ 0, 860 }; c.cursor_ddb = (struct intel_ddb){ 860, 892 };
    c.wm[0] = (struct intel_wm_level){ 1, 0, 8 };
    ntrace = 0;
    int rc = intel_hw_enable(&hw, &c, 0, 0);
    if (rc) fprintf(stderr, "HDMI: %s\n", hw.last_error);
    assert(rc == 0);
    assert(regs[0x6c048 / 4] == c.wrpll.cfgcr1 && regs[0x6c04c / 4] == c.wrpll.cfgcr2);
    assert(((regs[0x6c058 / 4] >> 12) & 0x3f) == (1u | (1u << 5)));                    /* DPLL2: override, HDMI */
    assert(first_write(0x6c04c, 0, 0) < first_write(0x46040, 1u << 31, 1u << 31));      /* programmed before enabled */
    assert(regs[(0x64e00 + 0x60 + 72) / 4] == intel_trans_hdmi[8].lo && regs[(0x64e04 + 0x60 + 72) / 4] == intel_trans_hdmi[8].hi);
    assert(((regs[0x61400 / 4] >> 24) & 7) == 0 && ((regs[0x61400 / 4] >> 28) & 7) == 1);   /* HDMI on DDI B */
    assert(regs[0x61200 / 4] & (1u << 12));                                            /* AVI enabled */
    uint8_t avi[17];
    intel_avi_infoframe(&c.timing, avi);
    /* 0x82+0x02+0x0d+0x10+0x28+0x10 = 0xd9, so the checksum is 0x27. */
    assert(avi[0] == 0x82 && avi[4] == 0x10 && avi[5] == 0x28 && avi[7] == 16 && avi[3] == 0x27);
    unsigned sum = 0;
    for (unsigned i = 0; i < 17; i++) sum += avi[i];
    assert((sum & 0xff) == 0);
    assert(regs[0x61220 / 4] == (uint32_t)(avi[0] | avi[1] << 8 | avi[2] << 16 | avi[3] << 24));
    assert(first_write(0x71008, 1u << 31, 1u << 31) < first_write(0x64100, 1u << 31, 1u << 31));   /* DDI buffer after */
    intel_hw_disable(&hw, &c, 0);
    assert(!(regs[0x64100 / 4] >> 31) && !(regs[0x46040 / 4] >> 31) && !(regs[0x61200 / 4] & (1u << 12)));
}

static void test_misc(void)
{
    reset();
    struct intel_hw hw;
    /* The firmware's own table for DDI A is kept, and its 10th entry allows swing 3. */
    for (unsigned i = 0; i < 20; i++) regs[(0x64e00 + i * 4) / 4] = 0x1000 + i;
    regs[0xc7208 / 4] = (2100u << 16) | 600;
    regs[0xc720c / 4] = (5000u << 16) | 400;
    regs[0xc8254 / 4] = 30000; regs[0xc8258 / 4] = 12000;
    regs[0xc8250 / 4] = 1u << 29;
    intel_hw_init(&hw, &sim, INTEL_PCH_CNP, 0);
    assert(hw.have_saved_trans[0] && !hw.have_saved_trans[1] && hw.pps.t1_t3 == 2100 && hw.pps.t9 == 400);
    assert(regs[0xc7208 / 4] == ((2100u << 16) | 600));                                /* firmware delays untouched */
    assert(hw.pwm_freq == 30000 && hw.pwm_duty == 12000 && hw.pwm_active_low);
    assert(intel_hw_backlight(&hw, 25) == 0 && regs[0xc8258 / 4] == 7500);
    assert(intel_hw_backlight(&hw, 101) == -EMBK_EINVAL);
    struct intel_crtc_cfg c = edp_cfg();
    c.keep_firmware_trans = 1;
    for (unsigned i = 0; i < 20; i++) regs[(0x64e00 + i * 4) / 4] = 0;
    need_swing = 3; need_preemph = 0;                                                   /* only a swing-3 table reaches it */
    assert(intel_hw_enable(&hw, &c, 0, 0) == 0);
    for (unsigned i = 0; i < 20; i++) assert(regs[(0x64e00 + i * 4) / 4] == 0x1000 + i);
    assert(((regs[0x64000 / 4] >> 24) & 0xf) == 9);                                    /* v3 p0 */
    intel_hw_disable(&hw, &c, 1);

    /* CDCLK: pcode prepares, then gets the voltage level. */
    nlevels = 0;
    assert(intel_hw_set_cdclk(&hw, 450000) == 0 && regs[0x46000 / 4] == intel_cdclk_ctl(450000));
    assert(cdclk_prepares >= 1 && nlevels == 1 && cdclk_levels[0] == 1 && hw.cdclk_khz == 450000);
    assert(intel_hw_set_cdclk(&hw, 500000) == -EMBK_EINVAL);
    pcode_reply = 0;
    assert(intel_hw_set_cdclk(&hw, 675000) == -EMBK_ETIMEDOUT && hw.cdclk_khz == 450000);

    /* A pixel clock above CDCLK is refused before anything is touched. */
    c = edp_cfg();
    c.timing.clock_khz = 500000;
    ntrace = 0;
    assert(intel_hw_enable(&hw, &c, 0, 0) == -EMBK_ERANGE);
    assert(first_write(0x6c05c, 0, 0) < 0);

    /* Hotplug detection turns itself on and reads the live state. */
    regs[0xc4000 / 4] = 1u << 21;
    assert(intel_hw_hpd_live(&hw, 1) == 1 && intel_hw_hpd_live(&hw, 2) == 0);
    assert((regs[0xc4030 / 4] & ((1u << 4) | (1u << 12) | (1u << 20))) == ((1u << 4) | (1u << 12) | (1u << 20)));
    regs[0x44418 / 4] = 1u << 31;
    assert(intel_hw_underrun(&hw, 1) == 1 && intel_hw_underrun(&hw, 1) == 0);

    /* GGTT: present bit, address, then the flush. */
    static uint64_t gsm[4];
    uint64_t pages[3] = { 0x12345000, 0x7ff00000, 0x100000000ull };
    ntrace = 0;
    intel_ggtt_map(&sim, gsm, pages, 3);
    assert(gsm[0] == (0x12345000ull | 1) && gsm[2] == (0x100000000ull | 1) && gsm[3] == 0);
    assert(trace[0].off == 0x101008 && trace[0].val == 1);
}

int main(void)
{
    test_edp();
    test_dp_and_fallback();
    test_hdmi();
    test_misc();
    puts("Intel display hardware: eDP enable/disable values and ordering, panel power and backlight, DP on "
         "DPLL1 with link fallback, failed training undone, HDMI WRPLL/translations/AVI infoframe, firmware "
         "tables kept, CDCLK pcode handshake, hotplug, underrun, GGTT OK");
    return 0;
}
