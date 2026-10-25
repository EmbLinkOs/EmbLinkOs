/* intel_regs.h -- how the Intel display code reaches the hardware.
 *
 * Every Intel display module (AUX, GMBUS, the capture, the modesetter) talks
 * to the device through this one small table instead of a mapped pointer, for
 * one reason: the host tests replace it with a simulated device -- a DP sink
 * answering AUX, an EDID ROM behind GMBUS -- and run the real protocol code
 * against it. A module that dereferenced the BAR directly could only ever be
 * tested on the laptop.
 *
 * Offsets are byte offsets into BAR0 (GTTMMADR), the same numbers Intel's
 * programmer's reference and Linux's i915 use. Display registers start at
 * 0x40000 and need no GT forcewake; nothing here touches the render engine. */
#ifndef EMBLINK_INTEL_REGS_H
#define EMBLINK_INTEL_REGS_H
#include <stdint.h>

struct intel_regs {
    uint32_t (*read)(void *ctx, uint32_t offset);
    void     (*write)(void *ctx, uint32_t offset, uint32_t value);
    uint64_t (*now_us)(void *ctx);             /* monotonic microseconds */
    void     *ctx;
};

static inline uint32_t intel_rd(const struct intel_regs *r, uint32_t off)
{
    return r->read(r->ctx, off);
}

static inline void intel_wr(const struct intel_regs *r, uint32_t off, uint32_t v)
{
    r->write(r->ctx, off, v);
}

/* Busy-wait. Display bring-up runs before the scheduler matters and inside
 * sequences with hard spec'd delays, so it spins on the clock rather than
 * sleeping. */
static inline void intel_udelay(const struct intel_regs *r, uint32_t us)
{
    uint64_t end = r->now_us(r->ctx) + us;
    while (r->now_us(r->ctx) < end) { }
}

/* PCH layouts, which decide the GMBUS pin numbering and the PWM registers. */
#define INTEL_PCH_UNKNOWN 0
#define INTEL_PCH_SPT     1        /* Sunrise Point / Kaby Point (100/200 series) */
#define INTEL_PCH_CNP     2        /* Cannon Point / Comet Point (300/400 series) */

#endif
