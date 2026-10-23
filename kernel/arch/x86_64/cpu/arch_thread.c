#include "include/arch_thread.h"
#include "arch/x86_64/cpu/fsbase.h"
#include "arch/x86_64/cpu/gdt.h"
#include "arch/x86_64/irq/lapic.h"

/* The x86_64 side of kernel/include/arch_thread.h. Three one-line functions,
 * and the point of them is that they are the ONLY reason the scheduler used to
 * include gdt.h, fsbase.h and lapic.h. */

void arch_tls_base_set(uint64_t base) {
    fsbase_set(base);
}

void arch_kernel_stack_set(uint64_t kstack_top) {
    /* Genuinely required here, on every single switch -- see the header. */
    tss_set_rsp0(kstack_top);
}

uint32_t arch_cpu_id(void) {
    return lapic_get_id();
}

void arch_enter_user_mode(uint64_t entry, uint64_t user_sp,
                          uint64_t a0, uint64_t a1, uint64_t a2)
{
    /* SysV puts the first three arguments in rdi, rsi, rdx -- so main(argc,
     * argv, envp) and a thread's single `void *arg` are the same three slots.
     *
     * They are loaded BEFORE the pushes, and rdi/rsi/rdx are in the clobber
     * list, which is what forces the compiler to pick other registers for the
     * five `"r"` operands below. Without that, one of the pushed values could
     * itself be allocated to rdi and the first three movq's would destroy it. */
    __asm__ volatile(
        "movq %4, %%rdi\n"
        "movq %5, %%rsi\n"
        "movq %6, %%rdx\n"
        "pushq %0\n"            /* ss     = user data selector | RPL 3      */
        "pushq %1\n"            /* rsp    = user stack top                  */
        "pushq $0x202\n"        /* rflags = IF=1: user code must be
                                  * interruptible, or a spinning program owns
                                  * the machine                              */
        "pushq %2\n"            /* cs     = user code selector | RPL 3      */
        "pushq %3\n"            /* rip    = entry point                     */
        "iretq\n"
        :
        : "r"((uint64_t)(0x18 | 3)), "r"(user_sp),
          "r"((uint64_t)(0x20 | 3)), "r"(entry), "r"(a0), "r"(a1), "r"(a2)
        : "rdi", "rsi", "rdx", "memory"
    );
    __builtin_unreachable();
}

void arch_fpu_pattern_load(const void *src16) {
    __asm__ volatile("movdqa (%0), %%xmm0" :: "r"(src16) : "memory");
}

void arch_fpu_pattern_store(void *dst16) {
    __asm__ volatile("movdqa %%xmm0, (%0)" :: "r"(dst16) : "memory");
}
