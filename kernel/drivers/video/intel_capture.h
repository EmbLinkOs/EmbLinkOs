/* intel_capture.h -- a read-only record of an Intel laptop's display, left on
 * the boot stick as /EMBKDISP.BIN.
 *
 * Native modesetting has to reproduce what the firmware did to light the
 * panel, on a machine this project cannot emulate and that has no serial
 * port. So before the driver writes a single mode register, the kernel
 * records everything the hardware and firmware will say: the PCI functions,
 * the ACPI OpRegion and its VBT (the board's own description of its ports and
 * panel), every display register block, each DP/eDP sink's DPCD and EDID over
 * AUX, each HDMI connector's EDID over GMBUS, the memory latencies pcode
 * reports for watermarks, and the GTT entries behind the firmware's
 * framebuffer. tools/intel_display_dump.py decodes it.
 *
 * The only writes it makes are protocol transactions -- AUX and GMBUS
 * requests, a pcode query -- the same ones Linux makes while the firmware's
 * mode is on screen. It never touches a mode, plane, pipe, PLL or power well.
 *
 * FORMAT (little-endian). "EMBKDISP", u32 version, u32 section count, then
 * sections of {u32 tag, u32 payload length, payload, zero padding to 4}:
 *
 *   1 PCI        u8 bus, dev, fn, role(0 GPU, 1 PCH, 2 other display); 256 B config
 *   2 BARS       u64 bar0, bar0_size, bar2, bar2_size; u32 pch_layout
 *   3 BOOTFB     u64 addr; u32 width, height, pitch, bpp, format, firmware, edid_size; 128 B EDID
 *   4 OPREGION   u64 phys; u32 length; bytes
 *   5 RVDA       u64 phys; u32 length; bytes (the VBT, when it lives outside mailbox 4)
 *   6 MMIO       u32 first offset; u32 count; u32 values[count]
 *   7 DPCD       u8 channel, 3 pad; u32 address; i32 rc; u32 length; bytes
 *   8 AUXEDID    u8 channel, 3 pad; i32 rc; u32 length; bytes
 *   9 GMBUSEDID  u8 pin, 3 pad; i32 rc; u32 length; bytes
 *  10 PCODE      u32 command; u32 parameter; i32 rc; u32 value
 *  11 GTT        u64 GTT phys; u32 first index; u32 framebuffer pages; u32 count; u64 PTEs[count]
 *  12 NOTE       text
 */
#ifndef EMBLINK_INTEL_CAPTURE_H
#define EMBLINK_INTEL_CAPTURE_H
#include <stdint.h>
#include "drivers/video/intel_regs.h"

#define INTEL_CAPTURE_PATH    "/EMBKDISP.BIN"
#define INTEL_CAPTURE_VERSION 1u
#define INTEL_CAPTURE_MAX     (512u * 1024u)
#define INTEL_CAPTURE_REGS    0x200000u    /* BAR0 bytes the capture reads: all registers */

enum {
    INTEL_CAP_PCI = 1, INTEL_CAP_BARS, INTEL_CAP_BOOTFB, INTEL_CAP_OPREGION,
    INTEL_CAP_RVDA, INTEL_CAP_MMIO, INTEL_CAP_DPCD, INTEL_CAP_AUXEDID,
    INTEL_CAP_GMBUSEDID, INTEL_CAP_PCODE, INTEL_CAP_GTT, INTEL_CAP_NOTE,
};

struct intel_capture_bdf { uint8_t bus, dev, fn, role; };

/* Everything the builder needs from the machine, so the host tests can hand
 * it a simulated one. */
struct intel_capture_env {
    const struct intel_regs *regs;          /* INTEL_CAPTURE_REGS of BAR0, mapped */
    uint32_t (*cfg_read32)(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
    const volatile uint8_t *(*map)(void *ctx, uint64_t phys, uint32_t length);
    void (*unmap)(void *ctx, const volatile uint8_t *virt, uint32_t length);
    void *ctx;

    struct intel_capture_bdf functions[8];  /* functions[0] is the GPU */
    unsigned function_count;
    uint64_t bar0, bar0_size, bar2, bar2_size;
    unsigned pch_layout;                    /* INTEL_PCH_* */

    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp, fb_format, firmware;
    uint32_t edid_size;
    const uint8_t *edid;                    /* the loader's 128-byte copy, or NULL */
};

/* What the capture found, for the one line the boot log gets. */
struct intel_capture_summary {
    uint32_t opregion_version;              /* major << 8 | minor, 0 = none */
    uint32_t vbt_bytes;
    uint32_t sections;
    uint8_t  dpcd_rev[4];                   /* per AUX channel, 0 = no sink */
    uint8_t  aux_edid;                      /* bit per AUX channel */
    uint8_t  gmbus_edid;                    /* bit per GMBUS pin */
    uint8_t  aux_bcd_powered;               /* power well 2 was on, so AUX B-D were probed */
    uint8_t  truncated;
};

/* Build the capture into buf. Returns its length (0 if cap is too small even
 * for the header). */
uint32_t intel_capture_build(const struct intel_capture_env *env, uint8_t *buf,
                             uint32_t cap, struct intel_capture_summary *sum);

/* Kernel: find an Intel display controller, capture it, and queue the result
 * for the boot stick. A machine without one costs one PCI scan. */
void intel_capture_run(void);

#endif
