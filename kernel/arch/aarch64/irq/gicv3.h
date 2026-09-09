#ifndef _AARCH64_GICV3_H
#define _AARCH64_GICV3_H

#include <stdint.h>
#include "include/types.h"

/* GICv3 -- the aarch64 interrupt controller. docs/ARM64.md phase A3.
 *
 * Replaces three x86 devices at once: the 8259 PIC (masking), the IOAPIC
 * (routing device interrupts to CPUs) and part of the LAPIC (per-CPU delivery
 * and end-of-interrupt). It is one device with three pieces:
 *
 *   distributor    (GICD, MMIO)   shared: routing and enable for SPIs
 *   redistributor  (GICR, MMIO)   one per CPU: enable for SGIs and PPIs
 *   CPU interface  (ICC_*_EL1)    SYSTEM REGISTERS, not MMIO -- this is the
 *                                 headline change from GICv2 and the reason
 *                                 acknowledging an interrupt costs no memory
 *                                 access at all
 *
 * Interrupt ID space, and the one thing to get right:
 *   0-15      SGI   software-generated, inter-processor (A9 uses these)
 *   16-31     PPI   per-CPU -- the generic timer lives here
 *   32-1019   SPI   shared peripherals -- virtio, PL011, PCIe
 *   1020-1023 special (1023 = spurious)
 *
 * A device tree says "PPI 11" or "SPI 3", NOT the INTID. The conversion is
 * gic_intid(); doing it by hand is how a timer ends up wired to the wrong
 * line and never fires. */

/* Device-tree interrupt types, from the ARM GIC binding. */
#define GIC_TYPE_SPI 0
#define GIC_TYPE_PPI 1

/* Convert a device tree (type, number) pair into an INTID. */
static inline uint32_t gic_intid(uint32_t type, uint32_t num) {
    return type == GIC_TYPE_PPI ? num + 16 : num + 32;
}

/* Bring up the distributor, this CPU's redistributor and this CPU's system
 * register interface. Reads the GICD/GICR addresses from the device tree.
 * Returns 0 on success; anything else means there is no usable controller and
 * the caller must not enable interrupts. */
int gic_init(void);

/* The per-core half: this core's redistributor and its ICC_* CPU interface.
 * gic_init() calls it for core 0; every secondary calls it for itself, because
 * every register involved is either in that core's own redistributor frame or
 * is a banked system register. Returns 0 on success. */
int gic_init_this_cpu(void);

/* Handlers take the INTID so one function can serve several lines. */
typedef void (*gic_handler_t)(uint32_t intid);

/* Register (and enable) / unregister (and disable) a handler for one INTID. */
int  gic_register(uint32_t intid, gic_handler_t handler, const char *name);

/* --- LPIs: what an MSI becomes -------------------------------------------
 * The ITS translates a device's memory write into an LPI INTID (8192 and up)
 * and targets a redistributor. These are separate from gic_register() because
 * LPIs are not in the SPI/PPI numbering and do not live in the same table. */
#define GIC_LPI_BASE 8192

int      gic_register_lpi(uint32_t intid, gic_handler_t handler, const char *name);
uint64_t gic_lpi_count(uint32_t intid);   /* deliveries, for the boot test */

/* This core's redistributor, PHYSICALLY -- the ITS targets them by address. */
uint64_t gic_redistributor_phys(void);

/* This core's GIC processor number (GICR_TYPER[23:8]) -- how the ITS names a
 * redistributor when GITS_TYPER.PTA is 0. Not the CPU index, not the MPIDR. */
uint32_t gic_processor_number(void);

/* ITS bring-up diagnostics -- see gicv3.c. */
uint32_t gic_redist_ctlr(void);
uint64_t gic_percpu_irq_count(uint32_t cpu);
int      gic_lpi_pending(uint32_t intid);
void gic_unregister(uint32_t intid);

/* Enable/disable one INTID at the controller without touching its handler. */
void gic_enable(uint32_t intid);
void gic_disable(uint32_t intid);

/* Work to run AFTER the interrupt has been fully retired, on the way out of
 * every interrupt. This is where a scheduler belongs, and the ordering is not
 * a detail -- see gic_dispatch() for why the handler, the EOI and this have to
 * happen in exactly this order. Pass 0 to clear. */
void gic_set_post_eoi(void (*fn)(void));

/* Called from the IRQ vector. Acknowledges, dispatches and ends the interrupt.
 * Not for general use. */
void gic_dispatch(void);

/* Per-INTID delivery counts, for the same reason kernel/arch/x86_64/irq/irq.h
 * keeps them: "the handler is registered" and "the interrupt is arriving" are
 * different claims, and only the second one is evidence. */
uint64_t gic_count(uint32_t intid);
uint64_t gic_spurious_count(void);
void     gic_dump(void);

#endif /* _AARCH64_GICV3_H */
