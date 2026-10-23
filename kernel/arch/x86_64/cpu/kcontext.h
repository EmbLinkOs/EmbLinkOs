#ifndef __KCONTEXT_H__
#define __KCONTEXT_H__

#include <stdint.h>

// A minimal saved kernel execution context (setjmp/longjmp style). With no
// scheduler yet, this is how a ring-3 task that calls exit() unwinds back into
// the kernel instead of halting: enter_user_mode saves a context, sys_exit
// restores it. Only the callee-saved registers, the stack pointer, and the
// resume address need saving. Field order MUST match the offsets used by
// kernel_ctx_save / kernel_ctx_restore in kcontext.asm.
struct kcontext {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip, rflags;
};

// setjmp: save the current context into ctx. Returns 0 on the direct call, and
// the (nonzero) value passed to kernel_ctx_restore when resumed.
uint64_t kernel_ctx_save(struct kcontext *ctx);

// longjmp: restore ctx so the matching kernel_ctx_save appears to return `val`
// (pass a nonzero value). Does not return to its own caller.
void kernel_ctx_restore(struct kcontext *ctx, uint64_t val);

/*
 * Switch from the current context to another context. Saves the current context
 * into `save_to` and restores the context from `restore_from`. This function
 * does not return to its caller; instead, it resumes execution at the point
 * where `restore_from` was saved.
 *
 * fpu_save_to/fpu_restore_from: each a pointer to a 512-byte, 16-byte-aligned
 * FXSAVE/FXRSTOR image (struct thread::fpu_state, process.h) -- saved/
 * restored alongside the GP-register context above via FXSAVE/FXRSTOR.
 * Requires fpu_init_this_cpu() (kernel/cpu/fpu.h) to have already run on
 * THIS core, or the FXSAVE/FXRSTOR themselves fault with #UD.
*/
/* Bytes, and alignment, of the area the two FP pointers above must address.
 * 512 is the FXSAVE image; FXSAVE/FXRSTOR #GP on an unaligned operand, hence
 * the 16.
 *
 * NAMED, rather than left as a literal 512 at the one place that allocates it,
 * because the aarch64 counterpart is a DIFFERENT SIZE (528: v0-v31 plus FPSR
 * and FPCR) and struct thread is shared. It was a literal, and aarch64's
 * FP_SAVE therefore wrote FPSR and FPCR sixteen bytes PAST the buffer -- over
 * struct thread's kstack_top and entry_point -- while FP_LOAD read FPCR back
 * out of a code address. An FPCR with arbitrary bits in it enables arbitrary
 * IEEE traps, which is how it announced itself: EC 0x2C, "trapped
 * floating-point exception", from a user program doing ordinary arithmetic. */
#define KCONTEXT_FPU_SIZE  512
#define KCONTEXT_FPU_ALIGN 16

void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from,
                        void *fpu_save_to, void *fpu_restore_from);

/* Fabricate a context that has never run, so the first schedule()-in lands
 * `entry` on `kstack_top` with interrupts off. The counterpart on aarch64 is
 * kernel/arch/aarch64/cpu/kcontext.h; the scheduler calls this one name.
 *
 * Replaces kernel/process/process.c poking ctx.rip/ctx.rsp/ctx.rflags by hand,
 * which is a thing only x86 has fields for -- and the two subtle constants it
 * needed (see the implementation) are properties of THIS machine's calling
 * convention and interrupt model, so they belong on this side of the line. */
void kernel_ctx_prepare(struct kcontext *ctx, void (*entry)(void),
                        uint64_t kstack_top);

/* Read-only views for diagnostics: where a parked thread would resume, and its
 * frame pointer, without the caller naming a register. The panic symbolizer
 * and the process viewer want these; nothing else should. */
uint64_t kernel_ctx_pc(const struct kcontext *ctx);
uint64_t kernel_ctx_fp(const struct kcontext *ctx);

#endif /* __KCONTEXT_H__ */
