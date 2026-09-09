#ifndef _X86_64_IRQFLAGS_H_
#define _X86_64_IRQFLAGS_H_

#include <stdint.h>
#include "include/types.h"

/* The x86_64 implementation of kernel/include/arch_irq.h. Include that, not
 * this. The flags word is RFLAGS and IF is bit 9. */

#define X86_RFLAGS_IF (1ULL << 9)

static inline uint64_t arch_irq_flags(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f) :: "memory");
    return f;
}

static inline uint64_t arch_irq_save(void) {
    uint64_t f;
    /* Read and mask in one asm block so nothing can be scheduled between them
     * -- reading, then disabling as a separate statement, leaves a window in
     * which an interrupt observes the old state. */
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void arch_irq_restore(uint64_t flags) {
    if (flags & X86_RFLAGS_IF)
        __asm__ volatile("sti" ::: "memory");
}

static inline bool arch_irq_enabled(void) {
    return (arch_irq_flags() & X86_RFLAGS_IF) != 0;
}

static inline void arch_irq_enable(void)  { __asm__ volatile("sti" ::: "memory"); }
static inline void arch_irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }

static inline void arch_cpu_idle(void) { __asm__ volatile("hlt" ::: "memory"); }

/* One asm block, so the sti/hlt pair keeps its interrupt shadow. See the
 * comment on the declaration in arch_irq.h. */
static inline void arch_cpu_idle_irq_on(void) {
    __asm__ volatile("sti; hlt" ::: "memory");
}

static inline void arch_cpu_halt_forever(void) {
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

static inline uint64_t arch_fault_addr(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

#endif /* _X86_64_IRQFLAGS_H_ */
