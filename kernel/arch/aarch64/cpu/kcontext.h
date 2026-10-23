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
 * THE FP STATE. Same four-argument shape as x86: two extra pointers to a
 * saved register file, which may be null for a context that has none. On x86
 * that area is a 512-byte FXSAVE image; here it is 528 bytes -- v0-v31 (512)
 * plus FPSR and FPCR -- and must be 16-byte aligned.
 *
 * Null is the honest value for every KERNEL thread: the kernel is compiled
 * -mgeneral-regs-only and cannot emit an FP instruction. It is not the honest
 * value for a user thread, and A5 is where that stops being hypothetical --
 * it opens CPACR_EL1.FPEN so EL0 may use FP at all, which is precisely why the
 * save/restore had to land in the same change rather than after it.
 *
 * Note the callee-saved rules do NOT shrink this the way they shrink the
 * general-register set: the AArch64 PCS preserves only the LOW 64 bits of
 * v8-v15 across a call and says nothing about the rest, so a preempted
 * thread's FP state is the whole file. */

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
 * to its caller only when something switches back. The two FP areas follow the
 * same outgoing/incoming pairing and may be null. */
void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from,
                       void *fpu_save_to, void *fpu_restore_from);

/* Bytes, and alignment, of the area the two FP pointers above must address. */
#define KCONTEXT_FPU_SIZE  528
#define KCONTEXT_FPU_ALIGN 16

/* Fabricate a context that has never run, so entering it calls `entry` on
 * `kstack_top`. Same name and shape as the x86 counterpart, so the shared
 * scheduler calls one function.
 *
 * Interrupts are ENABLED in the fabricated state, and that is not a free
 * choice: a thread first entered from inside an IRQ handler inherits
 * PSTATE.I set, and would then never be preempted again. (x86 does the
 * OPPOSITE for an equally specific reason -- see its implementation. The two
 * machines disagree because their trampolines reach spin_unlock differently,
 * which is precisely the sort of thing that must not be in shared code.) */
void kernel_ctx_prepare(struct kcontext *ctx, void (*entry)(void),
                        uint64_t kstack_top);

/* Read-only views for diagnostics; see the x86 header. */
uint64_t kernel_ctx_pc(const struct kcontext *ctx);
uint64_t kernel_ctx_fp(const struct kcontext *ctx);

#endif /* _AARCH64_KCONTEXT_H */
