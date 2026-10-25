/* intel_hw.h -- programming a Gen9 display pipeline: power wells, CDCLK,
 * DPLLs, DDI buffers, DP link training, eDP panel power and backlight,
 * transcoder, panel fitter, planes, watermarks, HDMI infoframes, GGTT.
 *
 * Everything here writes what intel_config.c decided, in the order Linux's
 * i915 uses for Skylake to Comet Lake (hsw_crtc_enable/disable,
 * intel_ddi_pre_enable_dp/hdmi, intel_dp_link_training.c, intel_pps.c).
 * The register model in tests/drivers/intel_hw_test.c checks both the values
 * and the ordering. */
#ifndef EMBLINK_INTEL_HW_H
#define EMBLINK_INTEL_HW_H
#include <stdint.h>
#include "drivers/video/intel_regs.h"
#include "drivers/video/intel_config.h"
#include "drivers/video/intel_vbt.h"

#define INTEL_PIPE_A 0u
#define INTEL_PIPE_B 1u
#define INTEL_PIPE_C 2u
#define INTEL_TRANS_EDP 3u            /* transcoders 0-2 are A-C */

/* Everything one lit output needs, decided before any register is written. */
struct intel_crtc_cfg {
    uint8_t  pipe, transcoder, ddi, kind, aux_ch, dpll;
    struct display_mode timing;       /* what leaves the transcoder (the panel's mode for eDP) */
    uint16_t src_w, src_h;            /* PIPESRC: the plane's size */
    uint8_t  pfit_enabled;
    struct intel_pfit pfit;
    uint8_t  bpc;                     /* 8, or 6 for a 6 bpc eDP panel (dithered) */

    /* DP / eDP */
    struct intel_dp_link link;
    struct intel_m_n m_n;
    uint8_t  enhanced_framing, downspread, tps3, training_interval;
    uint8_t  dpcd_rev;                /* DPCD 0x000: 1.4 sinks train with 100 us waits */
    /* HDMI / DVI */
    struct intel_wrpll wrpll;
    uint8_t  hdmi_sink, vic, hdmi_level;

    /* buffer translations: a port's table as the firmware programmed it is
     * kept; otherwise the built-in table for its kind */
    uint8_t  keep_firmware_trans;
    uint8_t  iboost;

    /* the plane */
    uint32_t surf_ggtt;               /* GGTT address, 256 KiB aligned */
    uint32_t stride;                  /* bytes, multiple of 64 */
    uint16_t plane_x, plane_y;        /* PLANE_OFFSET: where in a larger surface */
    uint8_t  rgb_order;               /* 1 = R in the low byte (FB_FORMAT_RGB) */
    struct intel_ddb plane_ddb, cursor_ddb;
    struct intel_wm_level wm[INTEL_WM_LEVELS];
};

/* eDP panel power: delays in 100 us units, as PP_*_DELAYS hold them. */
struct intel_pps {
    uint16_t t1_t3, t8, t9, t10, t11_t12;
};

/* Mutable state the hardware layer keeps between calls. */
struct intel_hw {
    const struct intel_regs *regs;
    unsigned pch;                     /* INTEL_PCH_* */
    uint32_t cdclk_khz, vco_khz;
    struct intel_pps pps;
    uint32_t pwm_freq;                /* backlight PWM period, in PWM clocks; 0 = unknown */
    uint32_t pwm_duty;
    uint8_t  pwm_active_low;
    uint32_t saved_trans[5][20];      /* each DDI's firmware buffer translations */
    uint8_t  have_saved_trans[5];
    uint8_t  dpll_users[4];           /* outputs using each DPLL */
    char     last_error[96];
};

/* Read what the firmware left that must be kept: CDCLK and its VCO, the
 * panel power delays (the VBT's where the registers hold none), backlight PWM,
 * and each DDI's buffer translations. Disables DC5/DC6. */
void intel_hw_init(struct intel_hw *hw, const struct intel_regs *regs, unsigned pch,
                   const struct intel_vbt *vbt);

/* Light one output from nothing: 0, or -errno with hw->last_error saying which
 * step failed. On failure everything this call turned on is turned off again.
 * `link_fallback` lets DP training retry at lower link configurations; the
 * caller's cfg is updated with the link that trained. */
int intel_hw_enable(struct intel_hw *hw, struct intel_crtc_cfg *cfg,
                    const struct intel_dp_caps *caps, const struct intel_dp_source *src);

/* Turn one output off, in reverse. keep_panel: leave an eDP panel powered (a
 * mode change follows at once, so its 500 ms power cycle is skipped). */
void intel_hw_disable(struct intel_hw *hw, const struct intel_crtc_cfg *cfg, int keep_panel);

/* Point a lit pipe's plane at another surface of the same size (a page
 * flip). Takes effect at the next vblank. */
void intel_hw_plane_flip(struct intel_hw *hw, const struct intel_crtc_cfg *cfg);

/* Re-program a lit pipe's plane whole -- surface, stride, offset, size -- with
 * its share of the display buffer and its watermarks, latched together at the
 * next vblank, which this waits for. For when another pipe comes or goes and
 * the buffer is split again: a pipe whose share shrinks must be moved before
 * the new pipe is given the blocks it gave up. */
void intel_hw_plane_update(struct intel_hw *hw, const struct intel_crtc_cfg *cfg);

/* Power for AUX channels B-D (they live in power well 2), so a monitor on an
 * external port can be asked what it is before anything is lit there. */
int intel_hw_aux_power(struct intel_hw *hw);

/* Wait for the pipe's next vblank (its frame counter to move; 50 ms at most). */
void intel_hw_wait_vblank(struct intel_hw *hw, unsigned pipe);

/* Raise CDCLK (or lower it) with the pcode handshake. Only with every pipe
 * off. 0 or -errno. */
int intel_hw_set_cdclk(struct intel_hw *hw, uint32_t cdclk_khz);

/* A pcode mailbox request (data in and out); 0 or -errno. */
int intel_hw_pcode_read(struct intel_hw *hw, uint32_t cmd, uint32_t *data);
#define INTEL_PCODE_READ_MEM_LATENCY 0x6u

/* eDP backlight: 0..100 percent of the PWM period. */
int intel_hw_backlight(struct intel_hw *hw, unsigned percent);

/* Hotplug: is something on this DDI's HPD line right now. Enables detection
 * on first use. */
int intel_hw_hpd_live(struct intel_hw *hw, unsigned ddi);

/* A pipe FIFO underrun since the last call (a watermark or bandwidth fault). */
int intel_hw_underrun(struct intel_hw *hw, unsigned pipe);

/* The DDI's buffer translation entries as currently programmed. */
void intel_hw_read_trans(const struct intel_regs *r, unsigned ddi, uint32_t out[20]);

/* The built-in tables (i915 intel_ddi_buf_trans.c, Kaby/Coffee/Comet Lake U):
 * pairs of {DDI_BUF_TRANS_LO, _HI} with an iboost value, and the HDMI table's
 * default entry. */
struct intel_trans_entry { uint32_t lo, hi; uint8_t iboost; };
extern const struct intel_trans_entry intel_trans_dp[9];
extern const struct intel_trans_entry intel_trans_hdmi[11];
#define INTEL_TRANS_HDMI_DEFAULT 8u

/* GGTT: write PTEs for `n` pages at GGTT page `first` through the mapped PTE
 * window `gsm` (the PTE for page `first` at offset 0), then flush. */
void intel_ggtt_map(const struct intel_regs *r, volatile uint64_t *gsm, const uint64_t *pages, uint32_t n);

/* The AVI infoframe for a mode, as the 17 bytes HSW+ DIP takes (3 header,
 * checksum, 13 payload). */
void intel_avi_infoframe(const struct display_mode *m, uint8_t out[17]);

#endif
