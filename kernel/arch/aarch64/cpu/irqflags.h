#ifndef _AARCH64_IRQFLAGS_H_
#define _AARCH64_IRQFLAGS_H_

#include <stdint.h>
#include "include/types.h"

/* The aarch64 implementation of kernel/include/arch_irq.h. Include that, not
 * this. The flags word is PSTATE.DAIF as `mrs` returns it: D=9, A=8, I=7, F=6,
 * and a SET bit means MASKED -- the opposite sense to x86's IF, which is
 * exactly why no caller outside these two headers may test a bit itself.
 *
 * Only I is touched. FIQ and SError stay masked or unmasked as they were:
 * this kernel routes nothing through FIQ, and unmasking SError in a critical
 * section would mean taking an asynchronous external abort in the one place
 * least able to handle it. */

#define AARCH64_DAIF_I (1UL << 7)

static inline uint64_t arch_irq_flags(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, daif" : "=r"(f) :: "memory");
    return f;
}

static inline uint64_t arch_irq_save(void) {
    uint64_t f;
    /* Read BEFORE masking. Unlike x86 there is no single instruction that does
     * both, but `msr daifset` is a write to a system register that cannot be
     * interrupted partway, so the pair is atomic enough: the worst case is an
     * interrupt taken between them, which observes the true old state. */
    __asm__ volatile("mrs %0, daif" : "=r"(f) :: "memory");
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return f;
}

static inline void arch_irq_restore(uint64_t flags) {
    /* Enable only if they were enabled -- see arch_irq.h on why this is not
     * symmetric. Note the inverted test against x86: a CLEAR I bit means
     * interrupts were ON. */
    if (!(flags & AARCH64_DAIF_I))
        __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline bool arch_irq_enabled(void) {
    return (arch_irq_flags() & AARCH64_DAIF_I) == 0;
}

static inline void arch_irq_enable(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void arch_irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

static inline void arch_cpu_idle(void) { __asm__ volatile("wfi" ::: "memory"); }

/* No interrupt-shadow problem to solve here: WFI is defined to return
 * immediately if a wake-up event is already pending, and an IRQ counts as one
 * even while PSTATE.I masks it. So the x86 hazard the shared declaration warns
 * about simply cannot occur -- the ordering below is for clarity, not safety. */
static inline void arch_cpu_idle_irq_on(void) {
    __asm__ volatile("msr daifclr, #2; wfi" ::: "memory");
}

static inline void arch_cpu_halt_forever(void) {
    for (;;)
        __asm__ volatile("msr daifset, #2; wfi" ::: "memory");
}

static inline uint64_t arch_fault_addr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

#endif /* _AARCH64_IRQFLAGS_H_ */
