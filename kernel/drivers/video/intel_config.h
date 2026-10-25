/* intel_config.h -- the arithmetic of a Gen9 modeset: which DP link, which
 * M/N ratios, which HDMI PLL dividers, which CDCLK, how the display buffer is
 * split and what the watermarks are, where the panel fitter puts the picture.
 *
 * Pure computation, no registers: every function here is host-tested against
 * values worked by hand from Intel's programming reference, following the
 * algorithms of Linux's i915 (intel_dp.c, intel_dpll_mgr.c skl_*, skl_watermark.c,
 * skl_scaler.c). The hardware layer (intel_hw.c) only programs what this
 * file decides. */
#ifndef EMBLINK_INTEL_CONFIG_H
#define EMBLINK_INTEL_CONFIG_H
#include <stdint.h>
#include "display/mode.h"

/* ---- DisplayPort ---------------------------------------------------------- */
#define INTEL_DP_MAX_RATES 8

/* What a sink says it can do (DPCD 0x000-0x00f, and the eDP 1.4 rate table at
 * 0x010-0x01f). Rates are link symbol clocks in kHz, as i915 counts them:
 * 162000 is 1.62 Gbps per lane. */
struct intel_dp_caps {
    uint8_t  revision;
    uint8_t  max_lanes;
    uint8_t  tps3, enhanced_framing, downspread;
    uint8_t  rate_table;             /* rates[] came from the eDP table: set by index */
    uint8_t  training_interval;      /* DPCD 0x00e bits 6:0 */
    uint8_t  n_rates;
    uint32_t rates[INTEL_DP_MAX_RATES];   /* ascending */
    uint8_t  rate_index[INTEL_DP_MAX_RATES];   /* each rate's position in the eDP table (DPCD 0x115) */
};

int intel_dp_caps_parse(const uint8_t dpcd[16], const uint8_t *edp_rates /* 16 bytes or NULL */,
                        struct intel_dp_caps *out);

struct intel_dp_link {
    uint32_t rate;                   /* link symbol clock, kHz */
    uint8_t  lanes;
    uint8_t  bpp;                    /* 24 or 18 */
    uint8_t  bw_code;                /* DPCD 0x100 value, 0 when set by rate_select */
    uint8_t  rate_select;            /* DPCD 0x115 index, 0xff when set by bw_code */
};

/* Source limits for this port. `rate_ok(rate)` may veto rates the PLL cannot
 * make (eDP on DPLL0 is tied to the CDCLK VCO). */
struct intel_dp_source {
    uint32_t max_rate;
    uint8_t  max_lanes;
    int    (*rate_ok)(uint32_t rate, void *ctx);
    void    *ctx;
};

/* eDP: the panel's own configuration -- the VBT's fast-link parameters when
 * given and possible, otherwise the highest common rate on every lane, which
 * is what panels are validated at. DP: the lowest rate, then the fewest
 * lanes, that carry the mode ("wide and slow", i915's order). 0 or -1. */
int intel_dp_pick_link(const struct intel_dp_caps *caps, const struct intel_dp_source *src,
                       const struct display_mode *mode, int edp, unsigned panel_bpp,
                       int vbt_rate_khz, unsigned vbt_lanes, struct intel_dp_link *out);

/* After a failed link training: the next configuration to try, or -1 when
 * nothing is left (i915 intel_dp_get_link_train_fallback_values: lower the
 * rate first, then halve the lanes). */
int intel_dp_link_fallback(const struct intel_dp_caps *caps, const struct intel_dp_source *src,
                           const struct display_mode *mode, struct intel_dp_link *link);

struct intel_m_n { uint32_t tu, data_m, data_n, link_m, link_n; };
void intel_link_m_n(unsigned bpp, unsigned lanes, uint32_t pixel_khz, uint32_t link_khz,
                    struct intel_m_n *out);

/* ---- PLLs and CDCLK --------------------------------------------------------- */
/* DPLL_CTRL1 link-rate codes for a DP link clock; -1 if the PLL has none. */
int intel_dpll_link_rate_code(uint32_t rate_khz);

/* An HDMI WRPLL (SKL DPLL1-3) for a TMDS clock. */
struct intel_wrpll {
    uint32_t dco_khz;                /* the DCO frequency chosen */
    uint32_t cfgcr1, cfgcr2;         /* ready for DPLLn_CFGCR1/2 */
    uint8_t  p0, p1, p2;
};
int intel_wrpll_compute(uint32_t clock_khz, struct intel_wrpll *out);
/* The TMDS clock a WRPLL configuration produces, kHz. */
uint32_t intel_wrpll_clock(uint32_t cfgcr1, uint32_t cfgcr2);

/* The lowest CDCLK (kHz) for this VCO (8100000 or 8640000 kHz) that is at
 * least `min_khz`; 0 if none is. */
uint32_t intel_cdclk_pick(uint32_t min_khz, uint32_t vco_khz);
/* CDCLK_CTL for a frequency, and the pcode voltage level it needs. */
uint32_t intel_cdclk_ctl(uint32_t cdclk_khz);
unsigned intel_cdclk_voltage_level(uint32_t cdclk_khz);

/* ---- display buffer and watermarks ------------------------------------------- */
#define INTEL_DDB_BLOCKS        892u     /* 896 on Gen9, 4 kept back for the bypass path */
#define INTEL_WM_LEVELS         8u
#define INTEL_CURSOR_DDB_SINGLE 32u      /* one pipe active */
#define INTEL_CURSOR_DDB_MULTI  8u

struct intel_ddb { uint16_t start, end; };   /* [start, end) in 512-byte blocks */

/* Split the DDB between active pipes in proportion to their widths, then each
 * pipe's share between its plane 1 and the cursor (fixed at the top). */
void intel_ddb_split(unsigned npipes, const unsigned *hdisplay,
                     struct intel_ddb *plane, struct intel_ddb *cursor);

struct intel_wm_level { uint8_t enabled; uint8_t lines; uint16_t blocks; };

/* Watermarks for a linear XRGB plane (skl_compute_plane_wm for Gen9):
 * `latency_us` from pcode, already adjusted (intel_wm_latency_adjust). Levels
 * whose blocks do not fit the allocation, and every level above them, are
 * disabled. Returns the highest enabled level, or -1 if level 0 does not fit. */
int intel_plane_wm(const uint16_t latency_us[INTEL_WM_LEVELS], uint32_t pixel_khz,
                   uint32_t htotal, uint32_t width, uint32_t cpp, uint32_t ddb_blocks,
                   struct intel_wm_level out[INTEL_WM_LEVELS]);

/* pcode's two 32-bit latency words to eight levels, with i915's rules: a zero
 * past level 0 disables it and every level after it; when level 0 reads 0,
 * every valid level gets the 2 us read latency pcode leaves out. */
void intel_wm_latency_adjust(uint32_t word0, uint32_t word1, uint16_t out[INTEL_WM_LEVELS]);

/* ---- the panel fitter ---------------------------------------------------------- */
struct intel_pfit { uint16_t x, y, w, h; uint16_t hphase, vphase; };
/* Where a src_w x src_h picture goes on a dst_w x dst_h panel, aspect kept,
 * centred, and the scaler's initial phases for it. -1 when the scaler cannot
 * (more than 2x downscale, or a side under 8 pixels). */
int intel_pfit_compute(unsigned src_w, unsigned src_h, unsigned dst_w, unsigned dst_h,
                       struct intel_pfit *out);

#endif
