#include "include/arch_ipi.h"
#include "arch/aarch64/smp/smp.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/cpu/percpu.h"
#include "include/kprintf.h"
#include "mm/vmm.h"

/* The aarch64 half of include/arch_ipi.h.
 *
 * An SGI ("software generated interrupt") is INTID 0-15, and unlike an x86 IPI
 * it is not a vector in a shared table: SGIs are per-core in the GIC exactly
 * as PPIs are, so each core enables its own. Sending one is the GIC
 * back-end's job: a system-register write on v3 (ICC_SGI1R_EL1), a
 * distributor write on v2 (GICD_SGIR).
 *
 * ONE THING THIS DOES NOT HAVE TO CARRY, and it is the interesting one:
 * IPI_TLB_SHOOTDOWN is never sent here. `tlbi ... is` broadcasts to the whole
 * inner-shareable domain in HARDWARE, so a page unmapped on one core is
 * already invalid on all of them by the time the `dsb ish` retires. x86 has to
 * send an interrupt and hope; this architecture simply does not have the
 * problem. The reason still exists in the enum because the SHARED code is
 * written against one set of meanings -- and because a machine with cores in
 * different shareability domains would need it.
 */

#define SGI_INTID(reason) ((uint32_t)(reason))   /* SGI 0, 1, 2 */

static void sgi_handler(uint32_t intid) {
    /* INTID IS the reason: SGIs 0..2 map one-to-one onto enum ipi_reason.
     * gic_dispatch() has already claimed the interrupt and will EOI it after
     * this returns -- except for IPI_HALT, which never returns, and that is
     * correct: a core parking on a panic has no use for a tidy EOI. */
    ipi_dispatch((enum ipi_reason)intid);
}

void arch_ipi_init_this_cpu(void) {
    for (uint32_t r = 0; r < IPI_REASON_COUNT; r++) {
        gic_register(SGI_INTID(r), sgi_handler, "IPI");
        gic_enable(SGI_INTID(r));
    }
}

bool arch_ipi_broadcast(enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT || cpu_count <= 1)
        return false;
    return gic_send_sgi_all_but_self(SGI_INTID(reason));
}

/* ONE core, by dense cpu index. How a core is NAMED to the GIC differs by
 * version -- an MPIDR affinity on v3, a CPU-interface bit on v2 -- and is the
 * GIC back-end's business (gic_hw.h). */
bool arch_ipi_send(uint32_t cpu, enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT || cpu >= cpu_count)
        return false;
    if (cpu == this_cpu()->cpu_index)
        return false;
    return gic_send_sgi(cpu, SGI_INTID(reason));
}

/* This core's TLB only. Present for the shared IPI handler's sake; nothing on
 * this architecture actually needs it, because the broadcast happens in
 * hardware -- see the note at the top. */
void arch_tlb_flush_all_local(void) {
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
