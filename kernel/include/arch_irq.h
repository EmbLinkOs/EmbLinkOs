#ifndef _ARCH_IRQ_H_
#define _ARCH_IRQ_H_

#include <stdint.h>
#include "include/types.h"

/* Interrupt masking on this CPU -- the first piece of the arch_* HAL, and
 * derived rather than invented (docs/ARM64.md §2.3).
 *
 * WHY THIS EXISTS. `__asm__ volatile("pushfq; popq %0; cli")` appeared as a
 * static inline in kernel/process/process.h, which half the kernel includes.
 * Measured: that ONE function blocked NINE shared source files and ~14,000
 * lines -- syscalls.c, fd.c, all of ipc/, embkfs.c -- from compiling for
 * aarch64, not because they use it, but because they include the header it
 * sits in. It was the single highest-leverage line in the tree.
 *
 * WHY IT IS A DISPATCH HEADER RATHER THAN A .c. These are two or three
 * instructions each and sit inside spinlocks and the scheduler's hot path; a
 * function call would cost more than the work. So each architecture supplies
 * its own inline versions and this header picks one -- the same shape Linux
 * uses for asm/irqflags.h, and the only place in the tree where an
 * architecture is selected by a compiler predefine rather than by the build.
 *
 * WHAT THE FLAGS WORD IS: opaque. x86 stores RFLAGS, aarch64 stores PSTATE.DAIF.
 * Nothing outside the per-arch headers may test a bit in it -- that was the
 * actual coupling, more than the asm itself: `if (flags & (1ULL << 9))` was
 * copy-pasted into five files, and bit 9 is a fact about one CPU.
 */

/* Current state, changing nothing. For code that wants to REMEMBER whether
 * interrupts were on so it can put them back after a path that loses them --
 * a blocking scheduler call has no iret/eret frame to restore them from. */
static inline uint64_t arch_irq_flags(void);

/* Current state, and disable. The classic critical-section entry. */
static inline uint64_t arch_irq_save(void);

/* Put back what arch_irq_flags()/arch_irq_save() captured.
 *
 * DELIBERATELY NOT SYMMETRIC: this ENABLES interrupts if the saved state had
 * them enabled, and otherwise does nothing. It never disables. That is the
 * semantic both existing implementations already had -- x86's spin_unlock and
 * the aarch64 one -- and it is the safe direction: a nested critical section
 * must not have interrupts switched on underneath it, and a caller that
 * genuinely wants them off calls arch_irq_disable(). */
static inline void arch_irq_restore(uint64_t flags);

/* Are interrupts enabled right now? For assertions and diagnostics -- several
 * places want to complain that a blocking path leaked a disabled state. */
static inline bool arch_irq_enabled(void);

static inline void arch_irq_enable(void);
static inline void arch_irq_disable(void);

/* A hint that this CPU is spinning on something another CPU will change.
 * `pause` / `yield`. Not a delay and not a barrier: it tells the core to stop
 * speculating so hard down a loop it is about to lose, which on x86 also
 * avoids the memory-order-violation pipeline flush when the value finally
 * lands. Omitting it is correct but slow; using it where there is no spin is
 * merely useless. */
static inline void arch_cpu_relax(void);

/* Stop the CPU until an interrupt arrives, WITHOUT changing the mask.
 * `hlt` / `wfi`. */
static inline void arch_cpu_idle(void);

/* Enable interrupts and idle, ATOMICALLY -- and the atomicity is the entire
 * point. On x86, `sti` has a one-instruction interrupt shadow, so `sti; hlt`
 * cannot take an interrupt between them; written as two separate statements,
 * an interrupt arriving in the gap leaves `hlt` waiting for one that has
 * already been delivered, and the core sleeps forever. That exact hang is
 * documented at ata.c's wait loop. Every "wait for the timer or the device"
 * loop in the kernel wants this, not the two calls. */
static inline void arch_cpu_idle_irq_on(void);

/* Stop this CPU permanently: mask interrupts and idle, forever. For the panic
 * path, where a hang here beats a hang somewhere less obvious. */
static inline void arch_cpu_halt_forever(void);

/* The faulting address of the exception being handled -- x86's CR2, aarch64's
 * FAR_EL1. Only meaningful inside a fault handler; garbage elsewhere. */
static inline uint64_t arch_fault_addr(void);

#if defined(__x86_64__)
#include "arch/x86_64/cpu/irqflags.h"
#elif defined(__aarch64__)
#include "arch/aarch64/cpu/irqflags.h"
#else
#error "arch_irq.h: no interrupt-masking implementation for this architecture"
#endif

#endif /* _ARCH_IRQ_H_ */
