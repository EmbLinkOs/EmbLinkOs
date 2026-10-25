/* intel_native.h -- native modesetting on Gen9 laptops: the outputs, the
 * framebuffers in the GGTT, and the gpu_driver hooks Settings drives.
 *
 * Boot keeps the firmware's picture when it is already the panel's native
 * mode -- nothing is reprogrammed, nothing flickers. Native modesetting
 * takes over the moment something needs it: a firmware mode that is not the
 * panel's own, a resolution chosen in Settings (lower ones are scaled by the
 * pipe's panel fitter), or an external monitor. Recovery and Diagnostics
 * boots never use it: they keep the firmware's display, which is the way out
 * if this driver ever fails a machine. */
#ifndef EMBLINK_INTEL_NATIVE_H
#define EMBLINK_INTEL_NATIVE_H
#include <stdint.h>
#include "drivers/video/gpu.h"

struct intel_native_probe {
    uint8_t  bus, dev, fn;
    uint16_t device_id;
    unsigned pch;
    volatile uint8_t *mmio;            /* BAR0, INTEL_NATIVE_REGS bytes mapped */
    uint64_t bar0, bar0_size, aperture, aperture_size;
    const struct gpu_mode *firmware;   /* the verified firmware scanout */
    unsigned firmware_pipe;
};
#define INTEL_NATIVE_REGS 0x200000u

/* Set native modesetting up. 0 when it is available, -errno when the driver
 * stays a firmware-handoff driver (with the reason in the boot log). */
int intel_native_init(const struct intel_native_probe *p);

/* The mode fb_init should start with: the firmware's when it is the panel's
 * native mode, otherwise a native modeset to it. */
int intel_native_boot_mode(struct gpu_mode *out);

uint32_t intel_native_pitch(uint32_t width);
/* The framebuffer a boot-time modeset left on screen (gpu_driver.resource_id). */
uint32_t intel_native_resource_id(void);
bool intel_native_prepare(struct gpu_target *t, void *back, uint64_t bytes, uint32_t w, uint32_t h);
bool intel_native_select(const struct gpu_target *t);
bool intel_native_release(const struct gpu_target *t);
void intel_native_describe(struct embk_monitor_info *info);

/* Several outputs (gpu_driver's output hooks): the panel is output 0, the
 * VBT's DP/HDMI ports follow. A layout lights at most three at once. */
unsigned intel_native_output_count(void);
bool intel_native_output_info(unsigned i, struct gpu_output *out);
int  intel_native_poll_outputs(void);
bool intel_native_apply_layout(const struct gpu_target *t, const struct gpu_layout *l);

#endif
