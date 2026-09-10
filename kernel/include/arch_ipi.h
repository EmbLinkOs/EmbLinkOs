#ifndef _ARCH_IPI_H_
#define _ARCH_IPI_H_

#include <stdint.h>
#include "include/types.h"

/* INTERRUPTING ANOTHER CORE.
 *
 * The one thing a multiprocessor kernel needs that a uniprocessor one has no
 * concept of: making a core that is busy elsewhere stop and do something. Both
 * machines can do it and neither calls it the same thing --
 *
 *   x86_64   an IPI, sent by writing the local APIC's Interrupt Command
 *            Register. The vector is an ordinary IDT vector.
 *   aarch64  an SGI ("software generated interrupt"), sent by writing
 *            ICC_SGI1R_EL1. Only 16 of them exist, INTID 0-15, and they are
 *            per-core in the GIC exactly as PPIs are.
 *
 * WHAT IT IS FOR, in the order the kernel needs it:
 *
 *   IPI_TLB_SHOOTDOWN  x86 only, and not a nicety. `invlpg` invalidates THIS
 *                      core's TLB and says nothing to the others, so a page
 *                      unmapped on one core stays mapped on every other until
 *                      something else evicts it -- a use-after-free the
 *                      hardware will happily perform. aarch64 does not need
 *                      it: `tlbi ... is` broadcasts to the whole
 *                      inner-shareable domain in hardware, which is one of the
 *                      genuinely better parts of that architecture.
 *   IPI_RESCHEDULE     make a core re-enter the scheduler now rather than at
 *                      its next tick -- what a wakeup on another core wants.
 *   IPI_HALT           stop every other core, for a panic. A panic that leaves
 *                      three cores running is a panic that keeps corrupting
 *                      whatever it was panicking about.
 */
enum ipi_reason {
    IPI_TLB_SHOOTDOWN = 0,
    IPI_RESCHEDULE,
    IPI_HALT,
    IPI_REASON_COUNT
};

/* Bring up this core's ability to receive IPIs. Called once per core, by that
 * core, during its own bring-up. */
void arch_ipi_init_this_cpu(void);

/* Send to every core EXCEPT this one. Returns false where the machine cannot
 * do it (no SGI support, one core) -- callers must treat that as "handle it
 * yourself", never as "it was delivered". */
bool arch_ipi_broadcast(enum ipi_reason reason);

/* ONE core, by dense cpu index. Waking a single idle core is not a broadcast
 * job: interrupting every other core to hand one thread to one of them is a
 * thundering herd, and measured on four cores it cost more than the 100 Hz
 * tick tickless idle was removing -- system idle fell from 98% to 87%. Returns
 * false for an out-of-range index or for this core itself. */
bool arch_ipi_send(uint32_t cpu, enum ipi_reason reason);

/* Called by the arch's interrupt path when an IPI arrives. Shared, so the
 * MEANING of each reason lives in one place and only the delivery differs. */
void ipi_dispatch(enum ipi_reason reason);

/* Deliveries observed, per reason -- for the boot self-test, which otherwise
 * cannot tell "sent" from "arrived". */
uint64_t ipi_count(enum ipi_reason reason);

#endif /* _ARCH_IPI_H_ */
