#ifndef _AARCH64_KCONTEXT_H
#define _AARCH64_KCONTEXT_H

#include <stdint.h>

/* Kernel context switching -- docs/ARM64.md phase A3.
 *
 * Same three functions, same semantics and the same NAMES as
 * kernel/arch/x86_64/cpu/kcontext.h, so the shared scheduler will call this
 * without knowing which machine it is on. Only the struct differs, and it
 * differs exactly as much as the two procedure-call standards do:
 *
 *     x86_64                       aarch64
 *     rbx rbp r12-r15              x19-x28, x29
 *     rsp                          sp
 *     rip (the return address)     pc  (x30 at the moment of saving)
 *     rflags                       DAIF
 *
 * Field order MUST match the offsets in kcontext.S. There is a _Static_assert
 * there in spirit and a comment here; the sizes are asserted in bringup.c.
 *
 * WHY THERE IS NO FPU ARGUMENT. The x86 kernel_ctx_switch() takes two extra
 * pointers to 512-byte FXSAVE images. This one does not, because the aarch64
 * kernel is compiled -mgeneral-regs-only and FP/SIMD is still TRAPPED at EL1
 * (CPACR_EL1.FPEN = 0) -- a kernel thread here has no floating-point state to
 * save, and adding a parameter that is always ignored would be a lie the
 * compiler cannot catch. A5 introduces EL0 threads, opens CPACR and adds the
 * FP half; docs/TODO.md records it. */

struct kcontext {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;        /* x29 */
    uint64_t sp;
    uint64_t pc;        /* resume address: x30 at the moment of saving */
    uint64_t daif;      /* PSTATE.DAIF -- the interrupt state to restore */
};

/* setjmp: save the current context. Returns 0 on the direct call and the
 * (nonzero) value passed to kernel_ctx_restore when resumed. */
uint64_t kernel_ctx_save(struct kcontext *ctx);

/* longjmp: resume ctx so its kernel_ctx_save appears to return `val`. */
void kernel_ctx_restore(struct kcontext *ctx, uint64_t val);

/* Save the current context into `save_to` and resume `restore_from`. Returns
 * to its caller only when something switches back. */
void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from);

/* Prepare a never-yet-run context: entering it calls fn(arg) on `stack_top`
 * with interrupts ENABLED. Interrupts matter -- a thread first entered from
 * inside an IRQ handler inherits PSTATE.I set, and would then never be
 * preempted again. */
void kernel_ctx_prepare(struct kcontext *ctx, void (*fn)(void *), void *arg,
                        uint64_t stack_top);

#endif /* _AARCH64_KCONTEXT_H */
