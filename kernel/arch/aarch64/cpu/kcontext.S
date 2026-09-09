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

/* Save / restore the FP+SIMD register file to a 528-byte, 16-byte-aligned
 * area: v0-v31 (512 bytes) then FPSR and FPCR.
 *
 * Callee-saved rules do NOT help here the way they do for the general
 * registers. The AArch64 PCS says only the LOW 64 bits of v8-v15 are preserved
 * across a call, and nothing at all about v0-v7 or v16-v31 -- so a thread's FP
 * state after a preemption is the whole register file, not a callee-saved
 * subset. There is no cheaper correct answer.
 *
 * A null pointer means "this context has no FP state", which is true of every
 * kernel thread: the kernel is built -mgeneral-regs-only and cannot generate
 * an FP instruction. It is not true of user threads, and A6 passes real
 * pointers for them. */
.macro FP_SAVE reg, skip
    cbz     \reg, \skip
    stp     q0,  q1,  [\reg, #16 * 0]
    stp     q2,  q3,  [\reg, #16 * 2]
    stp     q4,  q5,  [\reg, #16 * 4]
    stp     q6,  q7,  [\reg, #16 * 6]
    stp     q8,  q9,  [\reg, #16 * 8]
    stp     q10, q11, [\reg, #16 * 10]
    stp     q12, q13, [\reg, #16 * 12]
    stp     q14, q15, [\reg, #16 * 14]
    stp     q16, q17, [\reg, #16 * 16]
    stp     q18, q19, [\reg, #16 * 18]
    stp     q20, q21, [\reg, #16 * 20]
    stp     q22, q23, [\reg, #16 * 22]
    stp     q24, q25, [\reg, #16 * 24]
    stp     q26, q27, [\reg, #16 * 26]
    stp     q28, q29, [\reg, #16 * 28]
    stp     q30, q31, [\reg, #16 * 30]
    mrs     x9,  fpsr
    mrs     x10, fpcr
    /* STP's scaled 7-bit offset tops out at 504 for 64-bit registers, and the
     * status words sit at 512. One add, rather than reordering the layout to
     * suit the encoding. */
    add     x11, \reg, #512
    stp     x9, x10, [x11]
\skip:
.endm

.macro FP_LOAD reg, skip
    cbz     \reg, \skip
    ldp     q0,  q1,  [\reg, #16 * 0]
    ldp     q2,  q3,  [\reg, #16 * 2]
    ldp     q4,  q5,  [\reg, #16 * 4]
    ldp     q6,  q7,  [\reg, #16 * 6]
    ldp     q8,  q9,  [\reg, #16 * 8]
    ldp     q10, q11, [\reg, #16 * 10]
    ldp     q12, q13, [\reg, #16 * 12]
    ldp     q14, q15, [\reg, #16 * 14]
    ldp     q16, q17, [\reg, #16 * 16]
    ldp     q18, q19, [\reg, #16 * 18]
    ldp     q20, q21, [\reg, #16 * 20]
    ldp     q22, q23, [\reg, #16 * 22]
    ldp     q24, q25, [\reg, #16 * 24]
    ldp     q26, q27, [\reg, #16 * 26]
    ldp     q28, q29, [\reg, #16 * 28]
    ldp     q30, q31, [\reg, #16 * 30]
    add     x11, \reg, #512
    ldp     x9, x10, [x11]
    msr     fpsr, x9
    msr     fpcr, x10
\skip:
.endm

/* void kernel_ctx_switch(struct kcontext *save_to, struct kcontext *restore_from,
 *                        void *fpu_save_to, void *fpu_restore_from)
 *
 * Four arguments, matching kernel/arch/x86_64/cpu/kcontext.h exactly, so the
 * shared scheduler in process.c calls one signature on both machines. The FP
 * pointers may be null; see FP_SAVE. */
    .globl kernel_ctx_switch
kernel_ctx_switch:
    FP_SAVE x2, .Lfps_done
    CTX_SAVE x0
    CTX_LOAD x1
    FP_LOAD x3, .Lfpl_done
    ret

/* void aarch64_eret_to_el0(uint64_t entry, uint64_t user_sp,
 *                          uint64_t a0, uint64_t a1, uint64_t a2)
 *
 * The transition to EL0. Cannot be written in C: it ends in `eret` and never
 * returns to its caller.
 *
 * SP_EL0 is a SEPARATE REGISTER from the SP the kernel is using (we run at
 * EL1h, i.e. on SP_EL1), so setting the user stack does not disturb the kernel
 * one -- and when a syscall arrives, the vector builds its frame on SP_EL1,
 * still exactly where it was. That is the aarch64 equivalent of x86's
 * TSS.rsp0, except the hardware keeps both pointers instead of reloading one
 * from a table.
 *
 * SPSR = 0 means: EL0t, and DAIF all clear -- user code runs with interrupts
 * ENABLED. A user program entered with interrupts masked cannot be preempted
 * and owns the machine.
 *
 * Every other register is zeroed before the eret. Not tidiness: whatever is
 * left in them is kernel state, and handing user space a kernel pointer is
 * exactly the kind of leak that makes every other mitigation pointless. */
    .globl aarch64_eret_to_el0
aarch64_eret_to_el0:
    msr     sp_el0, x1
    msr     elr_el1, x0
    msr     spsr_el1, xzr

    /* The three user arguments arrive in x2/x3/x4 and must LEAVE in x0/x1/x2,
     * which overlaps -- so they are parked in registers the zeroing loop below
     * has not reached yet before anything is overwritten. */
    mov     x9,  x2
    mov     x10, x3
    mov     x11, x4

    mov     x0,  xzr
    mov     x1,  xzr
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr

    /* Now place the arguments, after everything else is zero. */
    mov     x0, x9
    mov     x1, x10
    mov     x2, x11
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    eret

/* The first instruction a brand-new thread ever executes.
 *
 * kernel_ctx_prepare() parks the entry function in x19 -- a callee-saved
 * register, so the context restore above delivers it for free and no stack
 * frame has to be forged. */
    .globl kthread_trampoline
kthread_trampoline:
    mov     x29, xzr                /* end the frame chain: a backtrace from a
                                     * thread's first frame must stop, not walk
                                     * into whatever the stack page held */
    mov     x30, xzr
    blr     x19
    b       kthread_exited          /* entry returned: C decides what that means */
