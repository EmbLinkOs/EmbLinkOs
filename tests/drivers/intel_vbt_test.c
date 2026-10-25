/* intel_vbt_test.c -- the VBT parser against the Comet Lake fixture, and
 * against VBTs that are wrong in the ways real ones are. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/video/intel_vbt.h"
#include "drivers/video/intel_regs.h"
#include "intel_fixtures.h"

static uint8_t vbt[0x2000];

static void fix_checksum(uint8_t *v)
{
    uint32_t n = (uint32_t)(v[24] | v[25] << 8);
    v[26] = 0;
    unsigned sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += v[i];
    v[26] = (uint8_t)(256 - (sum & 0xff));
}

int main(void)
{
    uint32_t len = make_vbt(vbt);
    struct intel_vbt v;
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == 0 && v.valid && v.bdb_version == 228);
    assert(v.child_count == 3);
    const struct intel_vbt_child *edp = intel_vbt_child_for_ddi(&v, 0);
    const struct intel_vbt_child *hdmi = intel_vbt_child_for_ddi(&v, 1);
    const struct intel_vbt_child *dp = intel_vbt_child_for_ddi(&v, 2);
    assert(edp && edp->kind == INTEL_OUT_EDP && edp->aux_ch == 0 && edp->gmbus_pin == 0);
    assert(hdmi && hdmi->kind == INTEL_OUT_HDMI && hdmi->aux_ch == INTEL_NO_AUX && hdmi->gmbus_pin == 1 && hdmi->hdmi_level == 8);
    assert(dp && dp->kind == INTEL_OUT_DP && dp->aux_ch == 2 && dp->gmbus_pin == 2 && dp->usb_type_c && !dp->dual_mode);
    assert(!intel_vbt_child_for_ddi(&v, 3));
    assert(v.panel_type == 2 && v.pps.valid && v.pps.t1_t3 == 2000 && v.pps.t11_t12 == 5000);
    assert(v.backlight.valid && v.backlight.pwm && v.backlight.pwm_hz == 200 && v.backlight.min_brightness == 10);
    assert(v.lfp_mode.hdisplay == 1920 && v.lfp_mode.vdisplay == 1080 && v.lfp_mode.clock_khz == 142500);
    assert(v.lfp_mode.flags == DISPLAY_MODE_HSYNC_POSITIVE);    /* VBT flag layout, not EDID's */
    assert(v.lfp_mode.hsync_start == 1968 && v.lfp_mode.hsync_end == 2000 && v.lfp_mode.htotal == 2080);

    /* The DDC pins of a Sunrise Point board are GMBUS pins as they stand. */
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_SPT, &v) == 0 && intel_vbt_child_for_ddi(&v, 1)->gmbus_pin == 1);
    /* Comet Point's DDI D is on GMBUS pin 4, and pin 3 is DDI F's. */
    uint8_t *child_c = vbt + 48 + 22 + 3 + 5 + 2 * 39;
    child_c[19] = 3;
    fix_checksum(vbt);
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == 0 && intel_vbt_child_for_ddi(&v, 2)->gmbus_pin == 4);
    child_c[19] = 4;
    fix_checksum(vbt);
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == 0 && intel_vbt_child_for_ddi(&v, 2)->gmbus_pin == 3);
    /* DP++: TMDS and DP signalling together. */
    child_c[2] = 0xd6; child_c[3] = 0x60;
    fix_checksum(vbt);
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == 0 && intel_vbt_child_for_ddi(&v, 2)->dual_mode);

    /* A corrupt VBT is ignored, not half-believed. */
    len = make_vbt(vbt);
    vbt[100] ^= 0x55;
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == -1 && !v.valid && v.child_count == 0);
    len = make_vbt(vbt);
    assert(intel_vbt_parse(vbt, len - 1, INTEL_PCH_CNP, &v) == -1);      /* says it is longer than it is */
    memcpy(vbt + 48, "BIOS_DATA_BLOCX ", 16); fix_checksum(vbt);
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == -1);
    /* A block whose size runs off the end stops the walk instead of overrunning. */
    len = make_vbt(vbt);
    vbt[48 + 22 + 1] = 0xff; vbt[48 + 22 + 2] = 0x7f; fix_checksum(vbt);
    assert(intel_vbt_parse(vbt, len, INTEL_PCH_CNP, &v) == 0 && v.child_count == 0 && v.panel_type == -1);

    /* The OpRegion locator: mailbox 4, RVDA relative (2.1) and absolute (2.0). */
    static uint8_t op[0x400 + 0x2000];          /* the fixture builder writes 8 KiB from 0x400 */
    memset(op, 0, sizeof op);
    memcpy(op, "IntelGraphicsMem", 16);
    len = make_vbt(op + 0x400);
    uint32_t off = 0, n = 0; uint64_t rvda = 0;
    assert(intel_vbt_locate(op, 0x8d000000, &off, &rvda, &n) == 1 && off == 0x400 && n == len);
    put32(op + 0x14, 2u << 24 | 1u << 16); put32(op + 0x58, 1u << 2);
    put32(op + 0x3ba, 0x2000); put32(op + 0x3c2, 0x1234);
    assert(intel_vbt_locate(op, 0x8d000000, &off, &rvda, &n) == 2 && rvda == 0x8d002000 && n == 0x1234);
    put32(op + 0x14, 2u << 24);
    put32(op + 0x3ba, 0x7f000000);
    assert(intel_vbt_locate(op, 0x8d000000, &off, &rvda, &n) == 2 && rvda == 0x7f000000);
    op[0] = 'X';
    assert(intel_vbt_locate(op, 0x8d000000, &off, &rvda, &n) == 0);
    puts("Intel VBT: children, eDP/HDMI/DP/DP++ kinds, AUX and CNP/SPT DDC pins, HDMI level, power sequence, "
         "backlight, panel timing in VBT flag layout, corrupt/truncated/overrunning VBTs, OpRegion VBT location OK");
    return 0;
}
