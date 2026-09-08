/* Kernel context switch -- docs/ARM64.md phase A3.
 *
 * Offsets MUST match struct kcontext in kcontext.h:
 *   x19..x28 at 0..72, fp(x29) 80, sp 88, pc 96, daif 104.  Size 112.
 *
 * Only callee-saved state is here, and that is not an optimisation: by the
 * AArch64 procedure call standard, x0-x18 are caller-saved, so whoever called
 * kernel_ctx_switch already spilled anything of theirs that mattered. Saving
 * them too would be 19 redundant stores on every preemption.
 *
 * The aarch64 version is markedly shorter than the x86 one and for a reason
 * worth noting: there is no equivalent of RFLAGS' arithmetic bits to preserve
 * (PSTATE.NZCV is caller-saved), and DAIF is the only processor state a
 * context actually owns. */

    .set K_FP,   80
    .set K_SP,   88
    .set K_PC,   96
    .set K_DAIF, 104

/* Save the callee-saved set from wherever we are into the context at \reg. */
.macro CTX_SAVE reg
    stp     x19, x20, [\reg, #0]
    stp     x21, x22, [\reg, #16]
    stp     x23, x24, [\reg, #32]
    stp     x25, x26, [\reg, #48]
    stp     x27, x28, [\reg, #64]
    str     x29,      [\reg, #K_FP]
    mov     x9, sp
    str     x9,       [\reg, #K_SP]
    /* x30 is the return address: resuming this context means returning to
     * whoever called us, which is precisely the setjmp contract. */
    str     x30,      [\reg, #K_PC]
    mrs     x9, daif
    str     x9,       [\reg, #K_DAIF]
.endm

.macro CTX_LOAD reg
    ldp     x19, x20, [\reg, #0]
    ldp     x21, x22, [\reg, #16]
    ldp     x23, x24, [\reg, #32]
    ldp     x25, x26, [\reg, #48]
    ldp     x27, x28, [\reg, #64]
    ldr     x29,      [\reg, #K_FP]
    ldr     x9,       [\reg, #K_SP]
    mov     sp, x9
    ldr     x30,      [\reg, #K_PC]
    ldr     x9,       [\reg, #K_DAIF]
    msr     daif, x9
.endm

    .text

/* uint64_t kernel_ctx_save(struct kcontext *ctx)   [ctx = x0] */
    .globl kernel_ctx_save
kernel_ctx_save:
    CTX_SAVE x0
    mov     x0, #0                  /* the direct call returns 0 */
    ret

/* void kernel_ctx_restore(struct kcontext *ctx, uint64_t val)  [x0, x1] */
    .globl kernel_ctx_restore
kernel_ctx_restore:
    CTX_LOAD x0
    /* Never let a restore appear to return 0: that is indistinguishable from
     * the direct call, and every caller branches on exactly that. */
    cmp     x1, #0
    csinc   x0, x1, xzr, ne         /* x0 = (x1 != 0) ? x1 : 1 */
    ret                             /* returns to the saved pc, now in x30 */

/* void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from) */
    .globl kernel_ctx_switch
kernel_ctx_switch:
    CTX_SAVE x0
    CTX_LOAD x1
    ret

/* The first instruction a brand-new thread ever executes.
 *
 * kernel_ctx_prepare() parks the entry function in x19 and its argument in
 * x20 -- callee-saved registers, so the context restore above delivers them
 * for free and no stack frame has to be forged. */
    .globl kthread_trampoline
kthread_trampoline:
    mov     x29, xzr                /* end the frame chain: a backtrace from a
                                     * thread's first frame must stop, not walk
                                     * into whatever the stack page held */
    mov     x30, xzr
    mov     x0, x20
    blr     x19
    b       kthread_exited          /* fn returned: C decides what that means */
