#ifndef _AARCH64_GIC_HW_H
#define _AARCH64_GIC_HW_H

#include <stdint.h>
#include "include/types.h"

/* The seam between irq/gic.c (what every GIC does) and the two hardware
 * back-ends, gicv3.c and gicv2.c (how THIS one does it) -- docs/RPI4.md P1.
 * Private to kernel/arch/aarch64/irq/; everything else uses gicv3.h.
 *
 * What is common is most of it: the handler table, the order of handler / EOI
 * / scheduler that gic_dispatch() depends on, the counters. What differs is
 * WHERE the registers are -- a GICv3's CPU interface is system registers and
 * each core has a redistributor; a GICv2's CPU interface is MMIO and the
 * per-core registers are banked in the distributor -- and how a CPU is named
 * when routing an SGI: an MPIDR affinity on v3, a GIC CPU-interface bit on v2.
 *
 * Chosen at RUNTIME from the device tree rather than per BOARD, because that
 * is what lets the GICv2 driver be exercised by the whole `virt` suite
 * (`-M virt,gic-version=2`), not only by the Pi's short boot. */

struct gic_hw {
    const char *name;

    /* Per-core half: run by each core on itself. 0 on success. */
    int      (*init_this_cpu)(void);

    void     (*enable)(uint32_t intid);
    void     (*disable)(uint32_t intid);

    /* Acknowledge: the raw IAR value, which is what eoi() must be given back
     * (on v2 an SGI's IAR carries the SOURCE CPU, and EOI needs it). */
    uint64_t (*ack)(void);
    uint32_t (*iar_intid)(uint64_t iar);
    void     (*eoi)(uint64_t iar);

    /* SGIs. False when the target cannot be addressed. */
    bool     (*sgi_all_but_self)(uint32_t sgi);
    bool     (*sgi_to_cpu)(uint32_t cpu_index, uint32_t sgi);
};

/* Each back-end: find its controller in the device tree, bring up the
 * machine-wide half (the distributor) and core 0's half. 0 and *out set on
 * success; -1 with nothing touched when the tree has no such controller;
 * -2 when it has one and it failed. */
int gicv3_probe(const struct gic_hw **out);
int gicv2_probe(const struct gic_hw **out);

/* v3 only: an LPI arrived (INTID >= GIC_LPI_BASE). Counts it and runs its
 * handler; the caller EOIs. */
void gicv3_lpi_dispatch(uint32_t intid);

#define GIC_MAX_INTID  1020
#define GIC_PRIORITY   0x80    /* see gic.c */

#endif
