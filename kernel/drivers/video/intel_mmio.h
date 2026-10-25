#ifndef EMBLINK_INTEL_MMIO_H
#define EMBLINK_INTEL_MMIO_H
#include "drivers/video/intel_regs.h"

/* Point `r` at a mapped BAR0, with the TSC as its clock. */
void intel_regs_init_mmio(struct intel_regs *r, volatile void *base);

#endif
