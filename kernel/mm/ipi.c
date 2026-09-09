#include "include/arch_ipi.h"
#include "include/arch_irq.h"
#include "include/kprintf.h"
#include "process/process.h"
#include "mm/vmm.h"     /* arch_tlb_flush_all_local() */

/* What an IPI MEANS, shared by both architectures. How it is delivered is
 * arch_ipi_broadcast()'s problem and differs completely; what to do when one
 * arrives does not differ at all, so it lives here. */

static volatile uint64_t g_counts[IPI_REASON_COUNT];

void ipi_dispatch(enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT)
        return;
    __atomic_add_fetch(&g_counts[reason], 1, __ATOMIC_RELAXED);

    switch (reason) {
    case IPI_TLB_SHOOTDOWN:
        /* The sender has already changed the page tables; this core only has
         * to stop trusting what it cached. A full local flush rather than a
         * single page: the alternative is passing an address alongside the
         * IPI, which needs a mailbox and a handshake, and a shootdown is rare
         * enough that the simple version is the right one until it is measured
         * to be wrong. */
        arch_tlb_flush_all_local();
        break;

    case IPI_RESCHEDULE:
        /* Nothing to do HERE. The interrupt itself is the whole point: it
         * drags this core out of whatever it was doing and into the interrupt
         * path, whose exit already runs the scheduler. Doing it in the handler
         * would be re-entering the scheduler from inside an interrupt, which
         * is the bug gic_dispatch()'s post-EOI hook exists to avoid. */
        break;

    case IPI_HALT:
        /* A panic elsewhere. Stop, and stay stopped: this core has nothing
         * useful left to contribute and every instruction it executes is
         * running against state somebody has already declared broken. */
        arch_irq_disable();
        for (;;)
            arch_cpu_idle();

    default:
        break;
    }
}

uint64_t ipi_count(enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT)
        return 0;
    return __atomic_load_n(&g_counts[reason], __ATOMIC_RELAXED);
}
