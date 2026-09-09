#include "include/arch_thread.h"

/* The aarch64 side of kernel/include/arch_thread.h. */

void arch_tls_base_set(uint64_t base) {
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(base));
}

void arch_kernel_stack_set(uint64_t kstack_top) {
    /* Nothing to do, and that is a FACT about this architecture rather than an
     * unimplemented stub -- see arch_thread.h. SP_EL1 is a register distinct
     * from SP_EL0, so taking an exception from user space does not have to
     * fetch a kernel stack pointer from anywhere: it is already in SP_EL1,
     * put there by kernel_ctx_switch when this thread was scheduled.
     *
     * x86 needs TSS.rsp0 precisely because it does NOT have a second stack
     * pointer, so the value has to live in a per-CPU table and be rewritten
     * whenever the per-CPU thread changes. */
    (void)kstack_top;
}

uint32_t arch_cpu_id(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    /* Affinity level 0 -- the core within its cluster, which is all a
     * single-cluster machine like QEMU `virt` uses. A multi-cluster part would
     * need Aff1 folded in, and would then also need this to stay dense, which
     * it would not be. */
    return (uint32_t)(mpidr & 0xFF);
}

void aarch64_eret_to_el0(uint64_t entry, uint64_t user_sp,
                         uint64_t a0, uint64_t a1, uint64_t a2);   /* kcontext.S */

void arch_enter_user_mode(uint64_t entry, uint64_t user_sp,
                          uint64_t a0, uint64_t a1, uint64_t a2)
{
    /* The whole transition is assembly: it ends in `eret` and never returns,
     * and every register that is not an argument has to be zeroed before the
     * program can look at it. */
    aarch64_eret_to_el0(entry, user_sp, a0, a1, a2);
    __builtin_unreachable();
}

/* q0 is the aarch64 counterpart of xmm0. These are the only FP instructions in
 * the kernel, and they are here rather than in shared code because the kernel
 * is compiled -mgeneral-regs-only precisely so the compiler cannot emit any. */
void arch_fpu_pattern_load(const void *src16) {
    __asm__ volatile("ldr q0, [%0]" :: "r"(src16) : "memory");
}

void arch_fpu_pattern_store(void *dst16) {
    __asm__ volatile("str q0, [%0]" :: "r"(dst16) : "memory");
}
