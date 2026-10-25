/* intel_hw.c -- program a Gen9 display pipeline. See intel_hw.h.
 *
 * Register offsets and bit fields: Intel's Skylake/Kaby Lake display PRM
 * (vol. 2c) as Linux's i915 encodes them (i915_reg.h and the intel_* display
 * headers). Sequences: i915's hsw_crtc_enable/disable, intel_ddi_pre_enable_*
 * / post_disable, intel_dp_link_training.c and intel_pps.c, in that order. */
#include "drivers/video/intel_hw.h"
#include "drivers/video/intel_dpaux.h"
#include "include/errno.h"

/* ---- registers ------------------------------------------------------------ */
#define PWR_WELL_CTL2          0x45404u
#define PW_REQ(i)              (2u << ((i) * 2))
#define PW_STATE(i)            (1u << ((i) * 2))
#define PW_DDI_A_E             1u
#define PW_2                   15u
#define FUSE_STATUS            0x42000u
#define FUSE_PG2_DIST          (1u << 25)
#define DC_STATE_EN            0x45504u

#define CDCLK_CTL              0x46000u
#define LCPLL1_CTL             0x46010u
#define LCPLL2_CTL             0x46014u
#define WRPLL_CTL1             0x46040u
#define WRPLL_CTL2             0x46060u
#define PLL_ENABLE             (1u << 31)
#define DPLL_CTRL1             0x6c058u
#define DPLL_CTRL2             0x6c05cu
#define DPLL_STATUS            0x6c060u
#define DPLL_CFGCR1(id)        (0x6c040u + ((id) - 1u) * 8u)
#define DPLL_CFGCR2(id)        (0x6c044u + ((id) - 1u) * 8u)
#define CTRL1_OVERRIDE(id)     (1u << ((id) * 6))
#define CTRL1_RATE(code, id)   ((uint32_t)(code) << ((id) * 6 + 1))
#define CTRL1_SSC(id)          (1u << ((id) * 6 + 4))
#define CTRL1_HDMI(id)         (1u << ((id) * 6 + 5))
#define CTRL1_MASK(id)         (0x3fu << ((id) * 6))
#define CTRL2_CLK_OFF(p)       (1u << ((p) + 15))
#define CTRL2_CLK_SEL(pll, p)  ((uint32_t)(pll) << ((p) * 3 + 1))
#define CTRL2_SEL_MASK(p)      (3u << ((p) * 3 + 1))
#define CTRL2_OVERRIDE(p)      (1u << ((p) * 3))
#define DPLL_LOCK(id)          (1u << ((id) * 8))

#define PCODE_MAILBOX          0x138124u
#define PCODE_DATA             0x138128u
#define PCODE_DATA1            0x13812cu
#define PCODE_READY            (1u << 31)
#define PCODE_CDCLK_CONTROL    0x7u
#define CDCLK_PREPARE          0x3u
#define CDCLK_READY            0x1u

#define DDI_BUF_CTL(p)         (0x64000u + (p) * 0x100u)
#define DDI_BUF_ENABLE         (1u << 31)
#define DDI_BUF_TRANS_SEL(n)   ((uint32_t)(n) << 24)
#define DDI_BUF_EMP_MASK       (0xfu << 24)
#define DDI_PORT_REVERSAL      (1u << 16)
#define DDI_BUF_IS_IDLE        (1u << 7)
#define DDI_A_4_LANES          (1u << 4)
#define DDI_PORT_WIDTH(w)      (((uint32_t)(w) - 1u) << 1)
#define DP_TP_CTL(p)           (0x64040u + (p) * 0x100u)
#define DP_TP_STATUS(p)        (0x64044u + (p) * 0x100u)
#define DP_TP_ENABLE           (1u << 31)
#define DP_TP_ENHANCED_FRAME   (1u << 18)
#define DP_TP_PAT1             (0u << 8)
#define DP_TP_PAT2             (1u << 8)
#define DP_TP_IDLE             (2u << 8)
#define DP_TP_NORMAL           (3u << 8)
#define DP_TP_PAT3             (4u << 8)
#define DP_TP_PAT_MASK         (7u << 8)
#define DP_TP_IDLE_DONE        (1u << 25)
#define DDI_TRANS_LO(p, i)     (0x64e00u + (p) * 0x60u + (i) * 8u)
#define DDI_TRANS_HI(p, i)     (0x64e04u + (p) * 0x60u + (i) * 8u)
#define DISPIO_BMU_CR0         0x6c00cu
#define BAL_SHIFT(p)           (8u + 3u * (p))
#define BAL_MASK(p)            (7u << BAL_SHIFT(p))
#define BAL_DISABLE(p)         (1u << (23u + (p)))

#define TRANS_BASE(t)          ((t) == INTEL_TRANS_EDP ? 0x6f000u : 0x60000u + (t) * 0x1000u)
#define TRANS_CONF(t)          ((t) == INTEL_TRANS_EDP ? 0x7f008u : 0x70008u + (t) * 0x1000u)
#define TRANS_CONF_ENABLE      (1u << 31)
#define TRANS_CONF_STATE       (1u << 30)
#define FUNC_ENABLE            (1u << 31)
#define FUNC_PORT(p)           ((uint32_t)(p) << 28)
#define FUNC_MODE_HDMI         (0u << 24)
#define FUNC_MODE_DVI          (1u << 24)
#define FUNC_MODE_DP_SST       (2u << 24)
#define FUNC_BPC_8             (0u << 20)
#define FUNC_BPC_6             (2u << 20)
#define FUNC_PVSYNC            (1u << 17)
#define FUNC_PHSYNC            (1u << 16)
#define FUNC_EDP_A_ON          (0u << 12)
#define FUNC_EDP_B_ONOFF       (5u << 12)
#define FUNC_EDP_C_ONOFF       (6u << 12)
#define FUNC_WIDTH(w)          (((uint32_t)(w) - 1u) << 1)
#define MSA_SYNC_CLK           (1u << 0)
#define MSA_6_BPC              (0u << 5)
#define MSA_8_BPC              (1u << 5)
#define DIP_CTL(t)             (TRANS_BASE(t) + 0x200u)
#define DIP_AVI(t, i)          (TRANS_BASE(t) + 0x220u + (i) * 4u)
#define DIP_ENABLE_AVI         (1u << 12)

#define PIPESRC(p)             (0x6001cu + (p) * 0x1000u)
#define PIPE_MISC(p)           (0x70030u + (p) * 0x1000u)
#define PIPE_MISC_BPC_6        (2u << 5)
#define PIPE_MISC_DITHER       (1u << 4)
#define PIPE_FRMCOUNT(p)       (0x70040u + (p) * 0x1000u)
#define WM_LINETIME(p)         (0x45270u + (p) * 4u)
#define PS_CTRL(p, i)          (0x68180u + (p) * 0x800u + (i) * 0x100u)
#define PS_WIN_POS(p, i)       (0x68170u + (p) * 0x800u + (i) * 0x100u)
#define PS_WIN_SZ(p, i)        (0x68174u + (p) * 0x800u + (i) * 0x100u)
#define PS_VPHASE(p, i)        (0x68188u + (p) * 0x800u + (i) * 0x100u)
#define PS_HPHASE(p, i)        (0x68194u + (p) * 0x800u + (i) * 0x100u)
#define PS_SCALER_EN           (1u << 31)

#define PLANE(p, off)          (0x70180u + (p) * 0x1000u + (off))   /* plane 1 */
#define PLANE_CTL_OFF          0x00u
#define PLANE_STRIDE_OFF       0x08u
#define PLANE_POS_OFF          0x0cu
#define PLANE_SIZE_OFF         0x10u
#define PLANE_SURF_OFF         0x1cu
#define PLANE_OFFSET_OFF       0x24u
#define PLANE_AUX_DIST_OFF     0x40u
#define PLANE_AUX_OFFSET_OFF   0x44u
#define PLANE_WM(p, l)         (0x70240u + (p) * 0x1000u + (l) * 4u)
#define PLANE_WM_TRANS(p)      (0x70268u + (p) * 0x1000u)
#define PLANE_BUF_CFG(p)       (0x7027cu + (p) * 0x1000u)
#define PLANE2_CTL(p)          (0x70280u + (p) * 0x1000u)
#define PLANE2_SURF(p)         (0x7029cu + (p) * 0x1000u)
#define PLANE2_BUF_CFG(p)      (0x7037cu + (p) * 0x1000u)
#define CUR_CTL(p)             (0x70080u + (p) * 0x1000u)
#define CUR_BASE(p)            (0x70084u + (p) * 0x1000u)
#define CUR_WM(p, l)           (0x70140u + (p) * 0x1000u + (l) * 4u)
#define CUR_WM_TRANS(p)        (0x70168u + (p) * 0x1000u)
#define CUR_BUF_CFG(p)         (0x7017cu + (p) * 0x1000u)
#define PLANE_CTL_ENABLE       (1u << 31)
#define PLANE_CTL_XRGB8888     (4u << 24)
#define PLANE_CTL_ORDER_RGBX   (1u << 20)
#define WM_EN                  (1u << 31)

#define PP_STATUS              0xc7200u
#define PP_CONTROL             0xc7204u
#define PP_ON_DELAYS           0xc7208u
#define PP_OFF_DELAYS          0xc720cu
#define PP_DIVISOR             0xc7210u
#define PP_ON                  (1u << 31)
#define PP_SEQ_MASK            (3u << 28)
#define PP_CYCLE_ACTIVE        (1u << 27)
#define PP_STATE_MASK          0xfu
#define PP_STATE_ON_IDLE       0x8u
#define PANEL_POWER_ON         (1u << 0)
#define PANEL_POWER_RESET      (1u << 1)
#define EDP_BLC_ENABLE         (1u << 2)
#define EDP_FORCE_VDD          (1u << 3)
#define CNP_CYCLE_DELAY_MASK   (0x1fu << 4)
#define BLC_CTL1               0xc8250u
#define BLC_CTL2               0xc8254u
#define BLC_DUTY_CNP           0xc8258u
#define BLC_ENABLE             (1u << 31)
#define BLC_POLARITY           (1u << 29)
#define SFUSE_STRAP            0xc2014u
#define SFUSE_RAW_24MHZ        (1u << 8)
#define SDEISR                 0xc4000u
#define SHOTPLUG_CTL           0xc4030u
#define SHOTPLUG_CTL2          0xc403cu
#define DE_PIPE_IIR(p)         (0x44408u + (p) * 0x10u)
#define DE_PIPE_UNDERRUN       (1u << 31)
#define GFX_FLSH_CNTL          0x101008u

/* DPCD addresses and bits (VESA DP 1.4). */
#define DPCD_LINK_BW_SET       0x100u
#define DPCD_TRAINING_PATTERN  0x102u
#define DPCD_DOWNSPREAD        0x107u
#define DPCD_LINK_RATE_SET     0x115u
#define DPCD_LANE_STATUS       0x202u
#define DPCD_SET_POWER         0x600u
#define TRAIN_SCRAMBLE_DISABLE 0x20u
#define TRAIN_MAX_SWING        (1u << 2)
#define TRAIN_MAX_PREEMPH      (1u << 5)
#define LANE_CR_DONE           0x1u
#define LANE_EQ_DONE           0x2u
#define LANE_SYMBOL_LOCKED     0x4u
#define INTERLANE_ALIGN        0x1u

/* ---- the built-in buffer translation tables ---------------------------------
 * Linux i915 intel_ddi_buf_trans.c: _kbl_u_trans_dp (Kaby, Coffee and Comet
 * Lake U, 9 levels) and _skl_trans_hdmi (default entry 8). A port the
 * firmware already programmed keeps the firmware's table instead -- those are
 * what the board was validated with. */
const struct intel_trans_entry intel_trans_dp[9] = {
    { 0x0000201b, 0x000000a1, 0x0 }, { 0x00005012, 0x00000088, 0x0 },
    { 0x80007011, 0x000000cd, 0x3 }, { 0x80009010, 0x000000c0, 0x3 },
    { 0x0000201b, 0x0000009d, 0x0 }, { 0x80005012, 0x000000c0, 0x3 },
    { 0x80007011, 0x000000c0, 0x3 }, { 0x00002016, 0x0000004f, 0x0 },
    { 0x80005012, 0x000000c0, 0x3 },
};
const struct intel_trans_entry intel_trans_hdmi[11] = {
    { 0x00000018, 0x000000ac, 0x0 }, { 0x00005012, 0x0000009d, 0x0 },
    { 0x00007011, 0x00000088, 0x0 }, { 0x00000018, 0x000000a1, 0x0 },
    { 0x00000018, 0x00000098, 0x0 }, { 0x00004013, 0x00000088, 0x0 },
    { 0x80006012, 0x000000cd, 0x1 }, { 0x00000018, 0x000000df, 0x0 },
    { 0x80003015, 0x000000cd, 0x1 }, { 0x80003015, 0x000000c0, 0x1 },
    { 0x80000018, 0x000000c0, 0x1 },
};

/* ---- helpers --------------------------------------------------------------- */
static void fail(struct intel_hw *hw, const char *what)
{
    unsigned i = 0;
    for (; what[i] && i < sizeof hw->last_error - 1; i++) hw->last_error[i] = what[i];
    hw->last_error[i] = 0;
}

static int wait_bits(const struct intel_regs *r, uint32_t reg, uint32_t mask, uint32_t want, uint32_t us)
{
    uint64_t deadline = r->now_us(r->ctx) + us;
    for (;;) {
        if ((intel_rd(r, reg) & mask) == want) return 0;
        if (r->now_us(r->ctx) >= deadline) return -EMBK_ETIMEDOUT;
    }
}

static void rmw(const struct intel_regs *r, uint32_t reg, uint32_t clear, uint32_t set)
{
    intel_wr(r, reg, (intel_rd(r, reg) & ~clear) | set);
}

static void wait_vblank(const struct intel_regs *r, unsigned pipe)
{
    uint32_t f = intel_rd(r, PIPE_FRMCOUNT(pipe));
    uint64_t deadline = r->now_us(r->ctx) + 50000;
    while (intel_rd(r, PIPE_FRMCOUNT(pipe)) == f && r->now_us(r->ctx) < deadline) { }
}

static unsigned io_well(unsigned ddi) { return ddi == 0 || ddi == 4 ? PW_DDI_A_E : ddi + 1u; }

static uint32_t pll_ctl(unsigned id)
{
    static const uint32_t ctl[4] = { LCPLL1_CTL, LCPLL2_CTL, WRPLL_CTL1, WRPLL_CTL2 };
    return ctl[id & 3];
}

static int pcode_rw(const struct intel_regs *r, uint32_t cmd, uint32_t *data, uint32_t us)
{
    if (intel_rd(r, PCODE_MAILBOX) & PCODE_READY) return -EMBK_EBUSY;
    intel_wr(r, PCODE_DATA, *data);
    intel_wr(r, PCODE_DATA1, 0);
    intel_wr(r, PCODE_MAILBOX, PCODE_READY | cmd);
    if (wait_bits(r, PCODE_MAILBOX, PCODE_READY, 0, us)) return -EMBK_ETIMEDOUT;
    if (intel_rd(r, PCODE_MAILBOX) & 0xffu) return -EMBK_EIO;
    *data = intel_rd(r, PCODE_DATA);
    return 0;
}

int intel_hw_pcode_read(struct intel_hw *hw, uint32_t cmd, uint32_t *data)
{
    return pcode_rw(hw->regs, cmd, data, 10000);
}

/* ---- init ------------------------------------------------------------------- */
void intel_hw_read_trans(const struct intel_regs *r, unsigned ddi, uint32_t out[20])
{
    for (unsigned i = 0; i < 10; i++) {
        out[2 * i] = intel_rd(r, DDI_TRANS_LO(ddi, i));
        out[2 * i + 1] = intel_rd(r, DDI_TRANS_HI(ddi, i));
    }
}

void intel_hw_init(struct intel_hw *hw, const struct intel_regs *r, unsigned pch, const struct intel_vbt *vbt)
{
    for (unsigned i = 0; i < sizeof *hw; i++) ((uint8_t *)hw)[i] = 0;
    hw->regs = r;
    hw->pch = pch;
    uint32_t cd = intel_rd(r, CDCLK_CTL);
    hw->cdclk_khz = (cd & 0x7ffu) * 500u + 1000u;
    /* DPLL0's link-rate field says which VCO feeds CDCLK: 1080 or 2160 is 8640. */
    unsigned code = (intel_rd(r, DPLL_CTRL1) >> 1) & 7u;
    hw->vco_khz = (code == 4 || code == 5) ? 8640000u : 8100000u;

    /* Panel power delays: the firmware's registers when it programmed them,
     * else the VBT's, else the eDP spec's maxima (i915 intel_pps_init_delays). */
    uint32_t on = intel_rd(r, PP_ON_DELAYS), off = intel_rd(r, PP_OFF_DELAYS);
    uint32_t cycle = pch == INTEL_PCH_CNP ? ((intel_rd(r, PP_CONTROL) >> 4) & 0x1fu) * 1000u
                                          : (intel_rd(r, PP_DIVISOR) & 0x1fu) * 1000u;
    struct intel_pps hwp = { (uint16_t)((on >> 16) & 0x1fff), (uint16_t)(on & 0x1fff),
                             (uint16_t)(off & 0x1fff), (uint16_t)((off >> 16) & 0x1fff), (uint16_t)cycle };
    struct intel_pps spec = { 2100, 500, 500, 5000, 5100 };
    struct intel_pps vb = spec;
    if (vbt && vbt->pps.valid) vb = (struct intel_pps){ vbt->pps.t1_t3, vbt->pps.t8, vbt->pps.t9, vbt->pps.t10, vbt->pps.t11_t12 };
    hw->pps.t1_t3 = hwp.t1_t3 ? hwp.t1_t3 : vb.t1_t3;
    hw->pps.t8 = hwp.t8 ? hwp.t8 : vb.t8;
    hw->pps.t9 = hwp.t9 ? hwp.t9 : vb.t9;
    hw->pps.t10 = hwp.t10 ? hwp.t10 : vb.t10;
    hw->pps.t11_t12 = hwp.t11_t12 ? hwp.t11_t12 : vb.t11_t12;
    if (!on || !off) {
        intel_wr(r, PP_ON_DELAYS, ((uint32_t)hw->pps.t1_t3 << 16) | hw->pps.t8);
        intel_wr(r, PP_OFF_DELAYS, ((uint32_t)hw->pps.t10 << 16) | hw->pps.t9);
    }
    if (pch == INTEL_PCH_CNP && !cycle)
        rmw(r, PP_CONTROL, CNP_CYCLE_DELAY_MASK, (uint32_t)((hw->pps.t11_t12 + 999u) / 1000u) << 4);

    /* Backlight PWM: keep the firmware's period; derive one from the VBT's
     * frequency only when there is none. */
    uint32_t ctl2 = intel_rd(r, BLC_CTL2);
    if (pch == INTEL_PCH_CNP) { hw->pwm_freq = ctl2; hw->pwm_duty = intel_rd(r, BLC_DUTY_CNP); }
    else { hw->pwm_freq = ctl2 >> 16; hw->pwm_duty = ctl2 & 0xffff; }
    hw->pwm_active_low = (intel_rd(r, BLC_CTL1) & BLC_POLARITY) != 0;
    if (!hw->pwm_freq && vbt && vbt->backlight.valid && vbt->backlight.pwm_hz) {
        uint32_t raw_khz = pch == INTEL_PCH_CNP && !(intel_rd(r, SFUSE_STRAP) & SFUSE_RAW_24MHZ) ? 19200u : 24000u;
        hw->pwm_freq = (raw_khz * 1000u + vbt->backlight.pwm_hz / 2) / vbt->backlight.pwm_hz;
        hw->pwm_active_low = vbt->backlight.active_low;
        hw->pwm_duty = hw->pwm_freq;
    }

    for (unsigned d = 0; d < 5; d++) {
        intel_hw_read_trans(r, d, hw->saved_trans[d]);
        for (unsigned i = 0; i < 20; i++) if (hw->saved_trans[d][i]) hw->have_saved_trans[d] = 1;
    }
    /* No DMC is loaded, so DC5/DC6 must stay off while this driver runs. */
    rmw(r, DC_STATE_EN, 3u, 0);
    /* DPLL0 feeds CDCLK and is never turned off by this driver. */
    hw->dpll_users[0] = 1;
}

/* ---- CDCLK ------------------------------------------------------------------ */
int intel_hw_set_cdclk(struct intel_hw *hw, uint32_t cdclk)
{
    const struct intel_regs *r = hw->regs;
    if (!intel_cdclk_pick(cdclk, hw->vco_khz) || intel_cdclk_pick(cdclk, hw->vco_khz) != cdclk) {
        fail(hw, "CDCLK: not a frequency of this VCO");
        return -EMBK_EINVAL;
    }
    if (cdclk == hw->cdclk_khz) return 0;
    /* skl_set_cdclk: ask pcode to get ready, for up to 3 ms. */
    uint64_t deadline = r->now_us(r->ctx) + 3000;
    for (;;) {
        uint32_t d = CDCLK_PREPARE;
        int rc = pcode_rw(r, PCODE_CDCLK_CONTROL, &d, 500);
        if (!rc && (d & CDCLK_READY)) break;
        if (r->now_us(r->ctx) >= deadline) { fail(hw, "CDCLK: pcode never became ready"); return -EMBK_ETIMEDOUT; }
    }
    intel_wr(r, CDCLK_CTL, intel_cdclk_ctl(cdclk));
    (void)intel_rd(r, CDCLK_CTL);
    uint32_t level = intel_cdclk_voltage_level(cdclk);
    if (pcode_rw(r, PCODE_CDCLK_CONTROL, &level, 2000)) {
        fail(hw, "CDCLK: pcode refused the voltage level");
        return -EMBK_EIO;
    }
    hw->cdclk_khz = cdclk;
    return 0;
}

/* ---- power wells ---------------------------------------------------------- */
static int power_on(struct intel_hw *hw, unsigned idx)
{
    const struct intel_regs *r = hw->regs;
    if (!(intel_rd(r, PWR_WELL_CTL2) & PW_REQ(idx))) rmw(r, PWR_WELL_CTL2, 0, PW_REQ(idx));
    if (wait_bits(r, PWR_WELL_CTL2, PW_STATE(idx), PW_STATE(idx), 10000)) {
        fail(hw, idx == PW_2 ? "power well 2 did not come up" : "DDI IO power well did not come up");
        return -EMBK_ETIMEDOUT;
    }
    if (idx == PW_2 && wait_bits(r, FUSE_STATUS, FUSE_PG2_DIST, FUSE_PG2_DIST, 10000)) {
        fail(hw, "power well 2 fuses did not distribute");
        return -EMBK_ETIMEDOUT;
    }
    return 0;
}

/* ---- PLLs ------------------------------------------------------------------ */
static int pll_enable(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    const struct intel_regs *r = hw->regs;
    unsigned id = c->dpll;
    uint32_t ctrl1 = CTRL1_OVERRIDE(id);
    if (c->kind == INTEL_OUT_HDMI || c->kind == INTEL_OUT_DVI) {
        if (id == 0) { fail(hw, "DPLL0 cannot drive HDMI"); return -EMBK_EINVAL; }
        ctrl1 |= CTRL1_HDMI(id);
    } else {
        int code = intel_dpll_link_rate_code(c->link.rate);
        if (code < 0) { fail(hw, "DPLL: no such DP link rate"); return -EMBK_EINVAL; }
        ctrl1 |= CTRL1_RATE((unsigned)code, id);
    }
    if (hw->dpll_users[id] > (id == 0 ? 1u : 0u) &&
        (intel_rd(r, DPLL_CTRL1) & CTRL1_MASK(id)) != ctrl1) {
        fail(hw, "DPLL: in use at another rate");
        return -EMBK_EBUSY;
    }
    rmw(r, DPLL_CTRL1, CTRL1_MASK(id), ctrl1);
    (void)intel_rd(r, DPLL_CTRL1);
    if (id == 0) {                                  /* always on; only its link rate changes */
        hw->dpll_users[0]++;
        return 0;
    }
    if (c->kind == INTEL_OUT_HDMI || c->kind == INTEL_OUT_DVI) {
        intel_wr(r, DPLL_CFGCR1(id), c->wrpll.cfgcr1);
        intel_wr(r, DPLL_CFGCR2(id), c->wrpll.cfgcr2);
        (void)intel_rd(r, DPLL_CFGCR2(id));
    }
    rmw(r, pll_ctl(id), 0, PLL_ENABLE);
    if (wait_bits(r, DPLL_STATUS, DPLL_LOCK(id), DPLL_LOCK(id), 5000)) {
        rmw(r, pll_ctl(id), PLL_ENABLE, 0);
        fail(hw, "DPLL did not lock");
        return -EMBK_ETIMEDOUT;
    }
    hw->dpll_users[id]++;
    return 0;
}

static void pll_disable(struct intel_hw *hw, unsigned id)
{
    if (hw->dpll_users[id]) hw->dpll_users[id]--;
    if (id == 0 || hw->dpll_users[id]) return;
    rmw(hw->regs, pll_ctl(id), PLL_ENABLE, 0);
    (void)intel_rd(hw->regs, pll_ctl(id));
}

static void ddi_clock(struct intel_hw *hw, unsigned ddi, unsigned pll, int on)
{
    const struct intel_regs *r = hw->regs;
    if (on) {
        rmw(r, DPLL_CTRL2, CTRL2_SEL_MASK(ddi) | CTRL2_CLK_OFF(ddi), CTRL2_OVERRIDE(ddi) | CTRL2_CLK_SEL(pll, ddi));
    } else {
        rmw(r, DPLL_CTRL2, 0, CTRL2_CLK_OFF(ddi));
    }
    (void)intel_rd(r, DPLL_CTRL2);
}

/* ---- buffer translations and iboost -------------------------------------- */
static void set_iboost(struct intel_hw *hw, unsigned ddi, unsigned iboost)
{
    const struct intel_regs *r = hw->regs;
    if (iboost && iboost != 1 && iboost != 3 && iboost != 7) return;   /* not a valid balance leg */
    uint32_t v = intel_rd(r, DISPIO_BMU_CR0) & ~(BAL_MASK(ddi) | BAL_DISABLE(ddi));
    v |= iboost ? iboost << BAL_SHIFT(ddi) : BAL_DISABLE(ddi);
    intel_wr(r, DISPIO_BMU_CR0, v);
}

static void program_trans(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    const struct intel_regs *r = hw->regs;
    unsigned d = c->ddi;
    if (c->keep_firmware_trans && hw->have_saved_trans[d]) {
        for (unsigned i = 0; i < 10; i++) {
            intel_wr(r, DDI_TRANS_LO(d, i), hw->saved_trans[d][2 * i]);
            intel_wr(r, DDI_TRANS_HI(d, i), hw->saved_trans[d][2 * i + 1]);
        }
        return;
    }
    if (c->kind == INTEL_OUT_HDMI || c->kind == INTEL_OUT_DVI) {
        unsigned level = c->hdmi_level < 11 ? c->hdmi_level : INTEL_TRANS_HDMI_DEFAULT;
        /* HDMI always uses entry 9 (i915 hsw_prepare_hdmi_ddi_buffers). */
        intel_wr(r, DDI_TRANS_LO(d, 9), intel_trans_hdmi[level].lo);
        intel_wr(r, DDI_TRANS_HI(d, 9), intel_trans_hdmi[level].hi);
        set_iboost(hw, d, c->iboost ? c->iboost : intel_trans_hdmi[level].iboost);
        return;
    }
    for (unsigned i = 0; i < 9; i++) {
        intel_wr(r, DDI_TRANS_LO(d, i), intel_trans_dp[i].lo);
        intel_wr(r, DDI_TRANS_HI(d, i), intel_trans_dp[i].hi);
    }
}

/* ---- eDP panel power and backlight ----------------------------------------- */
static uint32_t pp_control(struct intel_hw *hw) { return intel_rd(hw->regs, PP_CONTROL); }

static int panel_wait_cycle(struct intel_hw *hw)
{
    return wait_bits(hw->regs, PP_STATUS, PP_ON | PP_SEQ_MASK | PP_CYCLE_ACTIVE | PP_STATE_MASK, 0, 5000000);
}

static int panel_on(struct intel_hw *hw)
{
    const struct intel_regs *r = hw->regs;
    if ((intel_rd(r, PP_STATUS) & PP_ON) && (pp_control(hw) & PANEL_POWER_ON)) return 0;
    if (panel_wait_cycle(hw)) { fail(hw, "eDP: the panel's power cycle never finished"); return -EMBK_ETIMEDOUT; }
    intel_wr(r, PP_CONTROL, pp_control(hw) | PANEL_POWER_ON | PANEL_POWER_RESET);
    if (wait_bits(r, PP_STATUS, PP_ON | PP_SEQ_MASK | PP_STATE_MASK, PP_ON | PP_STATE_ON_IDLE, 5000000)) {
        fail(hw, "eDP: panel power did not come on");
        return -EMBK_ETIMEDOUT;
    }
    return 0;
}

static void panel_off(struct intel_hw *hw)
{
    const struct intel_regs *r = hw->regs;
    intel_wr(r, PP_CONTROL, pp_control(hw) & ~(PANEL_POWER_ON | PANEL_POWER_RESET | EDP_FORCE_VDD | EDP_BLC_ENABLE));
    (void)wait_bits(r, PP_STATUS, PP_ON | PP_SEQ_MASK, 0, 5000000);
}

static void pwm_write(struct intel_hw *hw)
{
    const struct intel_regs *r = hw->regs;
    if (hw->pch == INTEL_PCH_CNP) {
        intel_wr(r, BLC_CTL2, hw->pwm_freq);
        intel_wr(r, BLC_DUTY_CNP, hw->pwm_duty);
    } else {
        intel_wr(r, BLC_CTL2, (hw->pwm_freq << 16) | (hw->pwm_duty & 0xffff));
    }
}

static void backlight_on(struct intel_hw *hw)
{
    const struct intel_regs *r = hw->regs;
    if (hw->pwm_freq) {
        /* The brightness the user set since boot is in the duty register, not
         * in what this driver cached: keep it. */
        uint32_t live = hw->pch == INTEL_PCH_CNP ? intel_rd(r, BLC_DUTY_CNP) : intel_rd(r, BLC_CTL2) & 0xffffu;
        if (live && live <= hw->pwm_freq) hw->pwm_duty = live;
        uint32_t ctl1 = intel_rd(r, BLC_CTL1);
        if (ctl1 & BLC_ENABLE) intel_wr(r, BLC_CTL1, ctl1 & ~BLC_ENABLE);
        ctl1 = hw->pwm_active_low ? BLC_POLARITY : 0;
        intel_wr(r, BLC_CTL1, ctl1);
        if (!hw->pwm_duty) hw->pwm_duty = hw->pwm_freq;
        pwm_write(hw);
        intel_wr(r, BLC_CTL1, ctl1 | BLC_ENABLE);
    }
    intel_udelay(r, (uint32_t)hw->pps.t8 * 100u);         /* T8: video to backlight */
    intel_wr(r, PP_CONTROL, pp_control(hw) | EDP_BLC_ENABLE);
}

static void backlight_off(struct intel_hw *hw)
{
    const struct intel_regs *r = hw->regs;
    intel_wr(r, PP_CONTROL, pp_control(hw) & ~EDP_BLC_ENABLE);
    intel_udelay(r, (uint32_t)hw->pps.t9 * 100u);         /* T9: backlight to video off */
    uint32_t ctl1 = intel_rd(r, BLC_CTL1);
    if (ctl1 & BLC_ENABLE) intel_wr(r, BLC_CTL1, ctl1 & ~BLC_ENABLE);
}

int intel_hw_backlight(struct intel_hw *hw, unsigned percent)
{
    if (percent > 100 || !hw->pwm_freq) return -EMBK_EINVAL;
    uint32_t duty = (uint32_t)(((uint64_t)hw->pwm_freq * percent + 50) / 100);
    if (percent && !duty) duty = 1;
    hw->pwm_duty = duty;
    pwm_write(hw);
    return 0;
}

/* ---- DP link training ---------------------------------------------------- */
static unsigned signal_index(uint8_t t)
{
    static const uint8_t idx[4][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 0xff }, { 7, 8, 0xff, 0xff }, { 9, 0xff, 0xff, 0xff } };
    unsigned v = t & 3u, p = (t >> 3) & 3u, i = idx[v][p];
    return i == 0xff ? 0 : i;
}

static void source_levels(struct intel_hw *hw, const struct intel_crtc_cfg *c, uint32_t *buf, uint8_t train)
{
    unsigned i = signal_index(train);
    if (!c->keep_firmware_trans || !hw->have_saved_trans[c->ddi])
        set_iboost(hw, c->ddi, c->iboost ? c->iboost : (i < 9 ? intel_trans_dp[i].iboost : 0));
    *buf = (*buf & ~DDI_BUF_EMP_MASK) | DDI_BUF_TRANS_SEL(i);
    intel_wr(hw->regs, DDI_BUF_CTL(c->ddi), *buf);
    (void)intel_rd(hw->regs, DDI_BUF_CTL(c->ddi));
}

static void source_pattern(struct intel_hw *hw, const struct intel_crtc_cfg *c, uint32_t pattern)
{
    rmw(hw->regs, DP_TP_CTL(c->ddi), DP_TP_PAT_MASK, pattern);
    (void)intel_rd(hw->regs, DP_TP_CTL(c->ddi));
}

/* The highest swing this port drives: level 2 with the 9-entry DP table,
 * level 3 with a 10-entry (eDP) one -- and the most pre-emphasis allowed at
 * each swing. */
static unsigned max_swing(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    return c->keep_firmware_trans && hw->have_saved_trans[c->ddi] && hw->saved_trans[c->ddi][18] ? 3 : 2;
}

static uint8_t adjust(const uint8_t st[6], unsigned lanes, unsigned vmax)
{
    unsigned v = 0, p = 0;
    for (unsigned l = 0; l < lanes; l++) {
        unsigned req = (st[4 + l / 2] >> ((l & 1) * 4)) & 0xfu;
        if ((req & 3u) > v) v = req & 3u;
        if (((req >> 2) & 3u) > p) p = (req >> 2) & 3u;
    }
    uint8_t t = 0;
    if (v >= vmax) { v = vmax; t |= TRAIN_MAX_SWING; }
    unsigned pmax = 3 - v;
    if (p >= pmax) { p = pmax; t |= TRAIN_MAX_PREEMPH; }
    return (uint8_t)(t | v | (p << 3));
}

static int lanes_ok(const uint8_t st[6], unsigned lanes, unsigned bits)
{
    for (unsigned l = 0; l < lanes; l++)
        if (((st[l / 2] >> ((l & 1) * 4)) & bits) != bits) return 0;
    return 1;
}

static int dpcd_set_train(struct intel_hw *hw, const struct intel_crtc_cfg *c, uint8_t pattern, uint8_t train)
{
    uint8_t buf[5] = { pattern, train, train, train, train };
    return intel_dpcd_write(hw->regs, c->aux_ch, DPCD_TRAINING_PATTERN, buf, 1u + c->link.lanes);
}

static int link_train(struct intel_hw *hw, const struct intel_crtc_cfg *c, uint32_t *buf)
{
    const struct intel_regs *r = hw->regs;
    unsigned lanes = c->link.lanes, vmax = max_swing(hw, c);
    uint8_t st[6], train = 0;

    uint8_t cfg[2] = { c->link.bw_code, (uint8_t)(lanes | (c->enhanced_framing ? 0x80u : 0)) };
    if (intel_dpcd_write(r, c->aux_ch, DPCD_LINK_BW_SET, cfg, 2)) { fail(hw, "DP: sink refused the link configuration"); return -EMBK_EIO; }
    if (c->link.rate_select != 0xff &&
        intel_dpcd_write(r, c->aux_ch, DPCD_LINK_RATE_SET, &c->link.rate_select, 1)) {
        fail(hw, "eDP: sink refused the link rate");
        return -EMBK_EIO;
    }
    uint8_t spread[2] = { (uint8_t)(c->downspread ? 0x10u : 0u), 0x01u };   /* + ANSI 8b/10b */
    if (intel_dpcd_write(r, c->aux_ch, DPCD_DOWNSPREAD, spread, 2)) { fail(hw, "DP: sink refused downspread"); return -EMBK_EIO; }

    /* Clock recovery: TPS1, scrambling off, raising swing as the sink asks. */
    source_pattern(hw, c, DP_TP_PAT1);
    source_levels(hw, c, buf, train);
    if (dpcd_set_train(hw, c, 0x01u | TRAIN_SCRAMBLE_DISABLE, train)) { fail(hw, "DP: could not start training"); return -EMBK_EIO; }
    /* drm_dp_link_train_clock_recovery_delay: DP 1.4 sinks always 100 us. */
    unsigned interval = c->training_interval > 4 ? 4u : c->training_interval;
    unsigned cr_us = (interval == 0 || c->dpcd_rev >= 0x14) ? 100u : interval * 4000u;
    unsigned max_cr = c->dpcd_rev >= 0x14 ? 10u : 80u;
    int cr_ok = 0;
    unsigned same = 1;
    for (unsigned tries = 0; tries < max_cr; tries++) {
        intel_udelay(r, cr_us);
        if (intel_dpcd_read(r, c->aux_ch, DPCD_LANE_STATUS, st, 6)) { fail(hw, "DP: lane status unreadable"); return -EMBK_EIO; }
        if (lanes_ok(st, lanes, LANE_CR_DONE)) { cr_ok = 1; break; }
        if (same == 5) break;                       /* the same swing five times */
        if (train & TRAIN_MAX_SWING) break;         /* nothing left to raise */
        uint8_t before = train & 3u;
        train = adjust(st, lanes, vmax);
        same = (train & 3u) == before ? same + 1 : 1;
        source_levels(hw, c, buf, train);
        if (dpcd_set_train(hw, c, 0x01u | TRAIN_SCRAMBLE_DISABLE, train)) { fail(hw, "DP: training write failed"); return -EMBK_EIO; }
    }
    if (!cr_ok) { fail(hw, "DP: clock recovery failed"); return -EMBK_EIO; }

    /* Channel equalisation: TPS3 when the sink has it, TPS2 otherwise. */
    uint8_t pat = c->tps3 ? 0x03u : 0x02u;
    source_pattern(hw, c, c->tps3 ? DP_TP_PAT3 : DP_TP_PAT2);
    if (dpcd_set_train(hw, c, pat | TRAIN_SCRAMBLE_DISABLE, train)) { fail(hw, "DP: training write failed"); return -EMBK_EIO; }
    unsigned eq_us = interval == 0 ? 400u : interval * 4000u;
    int eq_ok = 0;
    for (unsigned tries = 0; tries < 5; tries++) {
        intel_udelay(r, eq_us);
        if (intel_dpcd_read(r, c->aux_ch, DPCD_LANE_STATUS, st, 6)) { fail(hw, "DP: lane status unreadable"); return -EMBK_EIO; }
        if (!lanes_ok(st, lanes, LANE_CR_DONE)) { fail(hw, "DP: clock recovery lost during equalisation"); return -EMBK_EIO; }
        if (lanes_ok(st, lanes, LANE_CR_DONE | LANE_EQ_DONE | LANE_SYMBOL_LOCKED) && (st[2] & INTERLANE_ALIGN)) {
            eq_ok = 1;
            break;
        }
        train = adjust(st, lanes, vmax);
        source_levels(hw, c, buf, train);
        if (dpcd_set_train(hw, c, pat | TRAIN_SCRAMBLE_DISABLE, train)) { fail(hw, "DP: training write failed"); return -EMBK_EIO; }
    }

    /* Idle pattern either way (i915 set_idle_link_train); port A has no idle-done. */
    source_pattern(hw, c, DP_TP_IDLE);
    if (c->ddi != 0) (void)wait_bits(r, DP_TP_STATUS(c->ddi), DP_TP_IDLE_DONE, DP_TP_IDLE_DONE, 8000);
    if (!eq_ok) { fail(hw, "DP: channel equalisation failed"); return -EMBK_EIO; }
    uint8_t off = 0;
    if (intel_dpcd_write(r, c->aux_ch, DPCD_TRAINING_PATTERN, &off, 1)) { fail(hw, "DP: could not end training"); return -EMBK_EIO; }
    source_pattern(hw, c, DP_TP_NORMAL);
    return 0;
}

static void ddi_buf_off(struct intel_hw *hw, unsigned ddi, int dp)
{
    const struct intel_regs *r = hw->regs;
    uint32_t buf = intel_rd(r, DDI_BUF_CTL(ddi));
    int was_on = (buf & DDI_BUF_ENABLE) != 0;
    intel_wr(r, DDI_BUF_CTL(ddi), buf & ~DDI_BUF_ENABLE);
    if (dp) {
        uint32_t tp = intel_rd(r, DP_TP_CTL(ddi));
        intel_wr(r, DP_TP_CTL(ddi), (tp & ~(DP_TP_ENABLE | DP_TP_PAT_MASK)) | DP_TP_PAT1);
    }
    if (was_on) (void)wait_bits(r, DDI_BUF_CTL(ddi), DDI_BUF_IS_IDLE, DDI_BUF_IS_IDLE, 8000);
}

/* ---- transcoder, pipe, plane ---------------------------------------------- */
static void timings(const struct intel_regs *r, const struct intel_crtc_cfg *c)
{
    const struct display_mode *m = &c->timing;
    uint32_t b = TRANS_BASE(c->transcoder);
    intel_wr(r, b + 0x00, ((uint32_t)(m->htotal - 1) << 16) | (uint32_t)(m->hdisplay - 1));
    intel_wr(r, b + 0x04, ((uint32_t)(m->htotal - 1) << 16) | (uint32_t)(m->hdisplay - 1));
    intel_wr(r, b + 0x08, ((uint32_t)(m->hsync_end - 1) << 16) | (uint32_t)(m->hsync_start - 1));
    intel_wr(r, b + 0x0c, ((uint32_t)(m->vtotal - 1) << 16) | (uint32_t)(m->vdisplay - 1));
    intel_wr(r, b + 0x10, ((uint32_t)(m->vtotal - 1) << 16) | (uint32_t)(m->vdisplay - 1));
    intel_wr(r, b + 0x14, ((uint32_t)(m->vsync_end - 1) << 16) | (uint32_t)(m->vsync_start - 1));
    intel_wr(r, b + 0x28, 0);                                    /* VSYNCSHIFT: progressive */
    intel_wr(r, PIPESRC(c->pipe), ((uint32_t)(c->src_w - 1) << 16) | (uint32_t)(c->src_h - 1));
}

static void pipe_misc_and_pfit(const struct intel_regs *r, const struct intel_crtc_cfg *c)
{
    intel_wr(r, PIPE_MISC(c->pipe), c->bpc == 6 ? (PIPE_MISC_BPC_6 | PIPE_MISC_DITHER) : 0);
    if (c->pfit_enabled) {
        intel_wr(r, PS_VPHASE(c->pipe, 0), c->pfit.vphase);
        intel_wr(r, PS_HPHASE(c->pipe, 0), c->pfit.hphase);
        intel_wr(r, PS_CTRL(c->pipe, 0), PS_SCALER_EN);
        intel_wr(r, PS_WIN_POS(c->pipe, 0), ((uint32_t)c->pfit.x << 16) | c->pfit.y);
        intel_wr(r, PS_WIN_SZ(c->pipe, 0), ((uint32_t)c->pfit.w << 16) | c->pfit.h);
    } else {
        intel_wr(r, PS_CTRL(c->pipe, 0), 0);
        intel_wr(r, PS_WIN_POS(c->pipe, 0), 0);
        intel_wr(r, PS_WIN_SZ(c->pipe, 0), 0);
    }
}

static void watermarks(const struct intel_regs *r, const struct intel_crtc_cfg *c)
{
    unsigned p = c->pipe;
    intel_wr(r, WM_LINETIME(p), (uint32_t)(((uint64_t)c->timing.htotal * 8000u + c->timing.clock_khz - 1) /
                                           c->timing.clock_khz) & 0x1ffu);
    intel_wr(r, PLANE_BUF_CFG(p), c->plane_ddb.end ? ((uint32_t)(c->plane_ddb.end - 1) << 16) | c->plane_ddb.start : 0);
    intel_wr(r, CUR_BUF_CFG(p), c->cursor_ddb.end ? ((uint32_t)(c->cursor_ddb.end - 1) << 16) | c->cursor_ddb.start : 0);
    intel_wr(r, PLANE2_BUF_CFG(p), 0);
    for (unsigned l = 0; l < INTEL_WM_LEVELS; l++) {
        const struct intel_wm_level *w = &c->wm[l];
        intel_wr(r, PLANE_WM(p, l), w->enabled ? WM_EN | ((uint32_t)w->lines << 14) | w->blocks : 0);
        intel_wr(r, CUR_WM(p, l), 0);
    }
    intel_wr(r, PLANE_WM_TRANS(p), 0);
    intel_wr(r, CUR_WM_TRANS(p), 0);
}

static void plane_write(const struct intel_regs *r, const struct intel_crtc_cfg *c)
{
    unsigned p = c->pipe;
    intel_wr(r, PLANE(p, PLANE_STRIDE_OFF), c->stride / 64u);
    intel_wr(r, PLANE(p, PLANE_POS_OFF), 0);
    intel_wr(r, PLANE(p, PLANE_SIZE_OFF), ((uint32_t)(c->src_h - 1) << 16) | (uint32_t)(c->src_w - 1));
    intel_wr(r, PLANE(p, PLANE_OFFSET_OFF), ((uint32_t)c->plane_y << 16) | c->plane_x);
    intel_wr(r, PLANE(p, PLANE_AUX_DIST_OFF), 0);
    intel_wr(r, PLANE(p, PLANE_AUX_OFFSET_OFF), 0);
    intel_wr(r, PLANE(p, PLANE_CTL_OFF), PLANE_CTL_ENABLE | PLANE_CTL_XRGB8888 | (c->rgb_order ? PLANE_CTL_ORDER_RGBX : 0));
    intel_wr(r, PLANE(p, PLANE_SURF_OFF), c->surf_ggtt);                 /* arms the update */
}

static void planes_off(const struct intel_regs *r, unsigned p)
{
    intel_wr(r, PLANE(p, PLANE_CTL_OFF), 0);
    intel_wr(r, PLANE(p, PLANE_SURF_OFF), 0);
    intel_wr(r, PLANE2_CTL(p), 0);
    intel_wr(r, PLANE2_SURF(p), 0);
    intel_wr(r, CUR_CTL(p), 0);
    intel_wr(r, CUR_BASE(p), 0);
}

static uint32_t func_ctl(const struct intel_crtc_cfg *c)
{
    uint32_t v = FUNC_ENABLE | (c->bpc == 6 ? FUNC_BPC_6 : FUNC_BPC_8);
    if (c->timing.flags & DISPLAY_MODE_VSYNC_POSITIVE) v |= FUNC_PVSYNC;
    if (c->timing.flags & DISPLAY_MODE_HSYNC_POSITIVE) v |= FUNC_PHSYNC;
    if (c->kind == INTEL_OUT_HDMI) v |= FUNC_MODE_HDMI;
    else if (c->kind == INTEL_OUT_DVI) v |= FUNC_MODE_DVI;
    else v |= FUNC_MODE_DP_SST | FUNC_WIDTH(c->link.lanes);
    if (c->transcoder == INTEL_TRANS_EDP)
        v |= c->pipe == 0 ? FUNC_EDP_A_ON : c->pipe == 1 ? FUNC_EDP_B_ONOFF : FUNC_EDP_C_ONOFF;
    else
        v |= FUNC_PORT(c->ddi);
    return v;
}

void intel_avi_infoframe(const struct display_mode *m, uint8_t f[17])
{
    for (unsigned i = 0; i < 17; i++) f[i] = 0;
    f[0] = 0x82; f[1] = 0x02; f[2] = 0x0d;              /* AVI, version 2, 13 bytes */
    unsigned aspect = 0;                                  /* picture aspect: 1 = 4:3, 2 = 16:9 */
    switch (m->vic) {
    case 1: case 2: case 17: aspect = 1; break;
    case 0: aspect = (uint32_t)m->hdisplay * 9u == (uint32_t)m->vdisplay * 16u ? 2 : 0; break;
    default: aspect = 2; break;
    }
    f[4] = 0x10;                                          /* PB1: RGB, active format present */
    f[5] = (uint8_t)((aspect << 4) | 0x8);               /* PB2: picture aspect; active = same */
    f[6] = 0x00;                                          /* PB3: default quantisation */
    f[7] = m->vic & 0x7f;                                 /* PB4: VIC */
    unsigned sum = 0;
    for (unsigned i = 0; i < 17; i++) sum += f[i];
    f[3] = (uint8_t)(256 - (sum & 0xff));
}

static void infoframes(const struct intel_regs *r, const struct intel_crtc_cfg *c)
{
    uint32_t ctl = intel_rd(r, DIP_CTL(c->transcoder)) & ~DIP_ENABLE_AVI;
    intel_wr(r, DIP_CTL(c->transcoder), ctl);
    if (c->kind != INTEL_OUT_HDMI || !c->hdmi_sink) return;
    uint8_t f[32] = { 0 };
    intel_avi_infoframe(&c->timing, f);
    for (unsigned i = 0; i < 8; i++)                      /* all 32 bytes, for the ECC */
        intel_wr(r, DIP_AVI(c->transcoder, i), (uint32_t)f[4 * i] | (uint32_t)f[4 * i + 1] << 8 |
                                               (uint32_t)f[4 * i + 2] << 16 | (uint32_t)f[4 * i + 3] << 24);
    intel_wr(r, DIP_CTL(c->transcoder), ctl | DIP_ENABLE_AVI);
}

static int transcoder_on(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    const struct intel_regs *r = hw->regs;
    intel_wr(r, TRANS_CONF(c->transcoder), TRANS_CONF_ENABLE);
    if (wait_bits(r, TRANS_CONF(c->transcoder), TRANS_CONF_STATE, TRANS_CONF_STATE, 100000)) {
        fail(hw, "the transcoder did not start");
        return -EMBK_ETIMEDOUT;
    }
    return 0;
}

static void transcoder_off(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    const struct intel_regs *r = hw->regs;
    rmw(r, TRANS_CONF(c->transcoder), TRANS_CONF_ENABLE, 0);
    (void)wait_bits(r, TRANS_CONF(c->transcoder), TRANS_CONF_STATE, 0, 100000);
    rmw(r, TRANS_BASE(c->transcoder) + 0x400u, FUNC_ENABLE, 0);
}

/* ---- the sequences ------------------------------------------------------- */
#define DONE_PW     0x01u
#define DONE_PLL    0x02u
#define DONE_CLOCK  0x04u
#define DONE_PANEL  0x08u
#define DONE_BUF    0x10u
#define DONE_TRANS  0x20u

static void undo(struct intel_hw *hw, const struct intel_crtc_cfg *c, unsigned done, int panel_was_on)
{
    int dp = c->kind == INTEL_OUT_DP || c->kind == INTEL_OUT_EDP;
    if (done & DONE_TRANS) { planes_off(hw->regs, c->pipe); transcoder_off(hw, c); }
    if (done & DONE_BUF) ddi_buf_off(hw, c->ddi, dp);
    if ((done & DONE_PANEL) && !panel_was_on) panel_off(hw);
    if (done & DONE_CLOCK) ddi_clock(hw, c->ddi, c->dpll, 0);
    if (done & DONE_PLL) pll_disable(hw, c->dpll);
}

static int enable_once(struct intel_hw *hw, struct intel_crtc_cfg *c, unsigned *done, int *panel_was_on)
{
    const struct intel_regs *r = hw->regs;
    int dp = c->kind == INTEL_OUT_DP || c->kind == INTEL_OUT_EDP;
    if (c->transcoder != INTEL_TRANS_EDP || c->pipe != INTEL_PIPE_A) {
        if (power_on(hw, PW_2)) return -EMBK_ETIMEDOUT;
    }
    if (power_on(hw, io_well(c->ddi))) return -EMBK_ETIMEDOUT;
    *done |= DONE_PW;
    if (c->timing.clock_khz > hw->cdclk_khz) { fail(hw, "CDCLK is below the pixel clock"); return -EMBK_ERANGE; }
    if (pll_enable(hw, c)) return -EMBK_EIO;
    *done |= DONE_PLL;
    ddi_clock(hw, c->ddi, c->dpll, 1);
    *done |= DONE_CLOCK;
    if (c->kind == INTEL_OUT_EDP) {
        *panel_was_on = (intel_rd(r, PP_STATUS) & PP_ON) != 0;
        if (panel_on(hw)) return -EMBK_ETIMEDOUT;
        *done |= DONE_PANEL;
    }
    program_trans(hw, c);

    uint32_t keep = intel_rd(r, DDI_BUF_CTL(c->ddi)) & (DDI_PORT_REVERSAL | DDI_A_4_LANES);
    if (c->ddi == 0 && c->link.lanes == 4) keep |= DDI_A_4_LANES;
    uint32_t buf = keep | DDI_BUF_ENABLE;
    if (dp) {
        uint8_t d0 = 1;
        (void)intel_dpcd_write(r, c->aux_ch, DPCD_SET_POWER, &d0, 1);   /* the sink to D0 */
        intel_udelay(r, 1000);
        intel_wr(r, DP_TP_CTL(c->ddi), DP_TP_ENABLE | DP_TP_PAT1 | (c->enhanced_framing ? DP_TP_ENHANCED_FRAME : 0));
        buf |= DDI_PORT_WIDTH(c->link.lanes) | DDI_BUF_TRANS_SEL(0);
        intel_wr(r, DDI_BUF_CTL(c->ddi), buf);
        (void)intel_rd(r, DDI_BUF_CTL(c->ddi));
        *done |= DONE_BUF;
        intel_udelay(r, 600);                            /* >518 us before the buffer is used */
        if (link_train(hw, c, &buf)) return -EMBK_EIO;
        intel_link_m_n(c->link.bpp, c->link.lanes, c->timing.clock_khz, c->link.rate, &c->m_n);
    }

    timings(r, c);
    if (dp) {
        uint32_t b = TRANS_BASE(c->transcoder);
        intel_wr(r, b + 0x30, ((c->m_n.tu - 1u) << 25) | c->m_n.data_m);
        intel_wr(r, b + 0x34, c->m_n.data_n);
        intel_wr(r, b + 0x40, c->m_n.link_m);
        intel_wr(r, b + 0x44, c->m_n.link_n);
        intel_wr(r, b + 0x410, MSA_SYNC_CLK | (c->bpc == 6 ? MSA_6_BPC : MSA_8_BPC));
    }
    pipe_misc_and_pfit(r, c);
    watermarks(r, c);
    infoframes(r, c);
    intel_wr(r, TRANS_BASE(c->transcoder) + 0x400u, func_ctl(c));
    if (transcoder_on(hw, c)) return -EMBK_ETIMEDOUT;
    *done |= DONE_TRANS;
    if (!dp) {
        /* HDMI: the buffer comes on after the transcoder (intel_enable_ddi_hdmi). */
        intel_wr(r, DDI_BUF_CTL(c->ddi), buf);
        (void)intel_rd(r, DDI_BUF_CTL(c->ddi));
        *done |= DONE_BUF;
    }
    plane_write(r, c);
    (void)intel_rd(r, DE_PIPE_IIR(c->pipe));
    intel_wr(r, DE_PIPE_IIR(c->pipe), DE_PIPE_UNDERRUN);  /* a clean slate for intel_hw_underrun */
    if (c->kind == INTEL_OUT_EDP) backlight_on(hw);
    return 0;
}

int intel_hw_enable(struct intel_hw *hw, struct intel_crtc_cfg *c,
                    const struct intel_dp_caps *caps, const struct intel_dp_source *src)
{
    hw->last_error[0] = 0;
    for (;;) {
        unsigned done = 0;
        int panel_was_on = 1;
        int rc = enable_once(hw, c, &done, &panel_was_on);
        if (!rc) return 0;
        undo(hw, c, done, panel_was_on);
        int dp = c->kind == INTEL_OUT_DP || c->kind == INTEL_OUT_EDP;
        /* Only a link that would not train is worth another configuration. */
        if (!dp || !caps || !src || !(done & DONE_BUF) || (done & DONE_TRANS) ||
            intel_dp_link_fallback(caps, src, &c->timing, &c->link))
            return rc;
    }
}

void intel_hw_disable(struct intel_hw *hw, const struct intel_crtc_cfg *c, int keep_panel)
{
    const struct intel_regs *r = hw->regs;
    int dp = c->kind == INTEL_OUT_DP || c->kind == INTEL_OUT_EDP;
    if (c->kind == INTEL_OUT_EDP) backlight_off(hw);
    planes_off(r, c->pipe);
    wait_vblank(r, c->pipe);
    transcoder_off(hw, c);
    intel_wr(r, PS_CTRL(c->pipe, 0), 0);
    intel_wr(r, PS_WIN_POS(c->pipe, 0), 0);
    intel_wr(r, PS_WIN_SZ(c->pipe, 0), 0);
    rmw(r, DIP_CTL(c->transcoder), DIP_ENABLE_AVI, 0);
    if (dp && c->kind == INTEL_OUT_DP) {
        uint8_t d3 = 2;
        (void)intel_dpcd_write(r, c->aux_ch, DPCD_SET_POWER, &d3, 1);
    }
    ddi_buf_off(hw, c->ddi, dp);
    if (c->kind == INTEL_OUT_EDP && !keep_panel) panel_off(hw);
    ddi_clock(hw, c->ddi, c->dpll, 0);
    pll_disable(hw, c->dpll);
}

void intel_hw_wait_vblank(struct intel_hw *hw, unsigned pipe)
{
    wait_vblank(hw->regs, pipe);
}

void intel_hw_plane_flip(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    const struct intel_regs *r = hw->regs;
    intel_wr(r, PLANE(c->pipe, PLANE_STRIDE_OFF), c->stride / 64u);
    intel_wr(r, PLANE(c->pipe, PLANE_OFFSET_OFF), ((uint32_t)c->plane_y << 16) | c->plane_x);
    intel_wr(r, PLANE(c->pipe, PLANE_SURF_OFF), c->surf_ggtt);
    wait_vblank(r, c->pipe);
}

void intel_hw_plane_update(struct intel_hw *hw, const struct intel_crtc_cfg *c)
{
    watermarks(hw->regs, c);
    plane_write(hw->regs, c);                 /* PLANE_SURF last: arms the whole update */
    wait_vblank(hw->regs, c->pipe);
}

int intel_hw_aux_power(struct intel_hw *hw)
{
    return power_on(hw, PW_2);
}

int intel_hw_hpd_live(struct intel_hw *hw, unsigned ddi)
{
    const struct intel_regs *r = hw->regs;
    static const uint8_t live_bit[5] = { 24, 21, 22, 23, 25 };
    if (ddi > 4) return 0;
    uint32_t want = (1u << 4) | (1u << 12) | (1u << 20) | (1u << 28);   /* B, C, D, A detection */
    uint32_t ctl = intel_rd(r, SHOTPLUG_CTL);
    /* Status bits are write-one-to-clear: write them back as zero. */
    if ((ctl & want) != want) intel_wr(r, SHOTPLUG_CTL, (ctl & want) | want);
    uint32_t ctl2 = intel_rd(r, SHOTPLUG_CTL2);
    if (!(ctl2 & (1u << 4))) intel_wr(r, SHOTPLUG_CTL2, (ctl2 & (1u << 4)) | (1u << 4));
    return (intel_rd(r, SDEISR) >> live_bit[ddi]) & 1;
}

int intel_hw_underrun(struct intel_hw *hw, unsigned pipe)
{
    uint32_t iir = intel_rd(hw->regs, DE_PIPE_IIR(pipe));
    if (!(iir & DE_PIPE_UNDERRUN)) return 0;
    intel_wr(hw->regs, DE_PIPE_IIR(pipe), DE_PIPE_UNDERRUN);
    return 1;
}

void intel_ggtt_map(const struct intel_regs *r, volatile uint64_t *gsm, const uint64_t *pages, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) gsm[i] = (pages[i] & 0x7ffffff000ull) | 1u;
    (void)gsm[n ? n - 1 : 0];
    intel_wr(r, GFX_FLSH_CNTL, 1u);
    (void)intel_rd(r, GFX_FLSH_CNTL);
}
