/* intel_mmio.c -- the kernel's struct intel_regs: a mapped BAR0 and the TSC
 * clock. The host tests provide their own. */
#include "drivers/video/intel_mmio.h"
#include "drivers/timer/timer.h"

static uint32_t mmio_read(void *ctx, uint32_t off)
{
    return *(volatile uint32_t *)((volatile uint8_t *)ctx + off);
}

static void mmio_write(void *ctx, uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = value;
}

static uint64_t mmio_now(void *ctx)
{
    (void)ctx;
    return time_get_us();
}

void intel_regs_init_mmio(struct intel_regs *r, volatile void *base)
{
    r->read = mmio_read;
    r->write = mmio_write;
    r->now_us = mmio_now;
    r->ctx = (void *)base;
}
