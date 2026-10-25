/* intel_vbt.h -- the Video BIOS Table: the board's own description of its
 * display outputs, published by the firmware in the ACPI OpRegion.
 *
 * It is the only place that says which DDI is the eDP panel and which is an
 * HDMI or USB-C port, which AUX channel and DDC pins each uses, the panel's
 * power-sequencing delays, its native timing, and how its backlight is driven.
 * Layouts follow Linux's i915 intel_vbt_defs.h. */
#ifndef EMBLINK_INTEL_VBT_H
#define EMBLINK_INTEL_VBT_H
#include <stdint.h>
#include "display/mode.h"

#define INTEL_OUT_NONE 0u
#define INTEL_OUT_EDP  1u
#define INTEL_OUT_DP   2u
#define INTEL_OUT_HDMI 3u
#define INTEL_OUT_DVI  4u

#define INTEL_NO_AUX   0xffu
#define INTEL_VBT_MAX_CHILDREN 8u

struct intel_vbt_child {
    uint16_t device_type;
    uint8_t  kind;              /* INTEL_OUT_* */
    uint8_t  ddi;               /* 0 = A ... 5 = F; 0xff unknown */
    uint8_t  aux_ch;            /* 0 = A ... 3 = D; INTEL_NO_AUX */
    uint8_t  gmbus_pin;         /* already mapped for the PCH; 0 = none */
    uint8_t  hdmi_level;        /* buffer-translation entry for HDMI; 0xff = the table's default */
    uint8_t  dp_max_link_rate;  /* VBT code; 0 = no limit */
    uint8_t  usb_type_c, thunderbolt, lane_reversal, hpd_invert;
    uint8_t  iboost, dp_iboost_level, hdmi_iboost_level;
    uint8_t  dual_mode;         /* DP++: a passive HDMI adaptor is allowed */
};

struct intel_vbt {
    int      valid;
    uint16_t bdb_version;
    unsigned child_count;
    struct intel_vbt_child children[INTEL_VBT_MAX_CHILDREN];

    int      panel_type;        /* -1 when the VBT names none */
    /* eDP panel power sequencing, in units of 100 us (VBT block 27). */
    struct { uint16_t t1_t3, t8, t9, t10, t11_t12; uint8_t valid; } pps;
    uint8_t  edp_bpp;           /* 18, 24, 30 or 36; 0 unknown */
    uint8_t  edp_low_vswing;    /* use the eDP low-swing translation table */
    struct { uint8_t rate_code, lanes, valid; } edp_fast_link;   /* rate: 0 1.62, 1 2.7, 2 5.4 Gbps */

    struct { uint8_t pwm, active_low, min_brightness, valid; uint16_t pwm_hz; } backlight;

    struct display_mode lfp_mode;   /* the panel's timing (block 42); clock_khz 0 if none */
};

/* Parse a complete VBT ("$VBT" header onwards). pch_layout (INTEL_PCH_*)
 * decides how DDC pin numbers map to GMBUS pins. 0 or -1. */
int intel_vbt_parse(const uint8_t *vbt, uint32_t len, unsigned pch_layout, struct intel_vbt *out);

/* The child for a DDI, or NULL. */
const struct intel_vbt_child *intel_vbt_child_for_ddi(const struct intel_vbt *vbt, unsigned ddi);

/* Where the VBT sits in an OpRegion: in mailbox 4 (`offset` and `len` inside
 * the 8 KiB OpRegion), or as raw VBT data elsewhere (`rvda_phys` and `len`).
 * Returns 1 for mailbox 4, 2 for RVDA, 0 for no VBT. `opregion` is the mapped
 * 8 KiB. */
#define INTEL_OPREGION_SIZE 0x2000u
int intel_vbt_locate(const volatile uint8_t *opregion, uint64_t opregion_phys,
                     uint32_t *offset, uint64_t *rvda_phys, uint32_t *len);

#endif
