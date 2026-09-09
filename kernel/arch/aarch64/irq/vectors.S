/* The EL1 exception vector table -- docs/ARM64.md phase A1.
 *
 * VBAR_EL1 points here. The hardware indexes this table by (which stack the
 * exception came from, what kind it is) and jumps to a FIXED OFFSET: entries
 * are 128 bytes apart and the table must be 2 KiB aligned. That is 32
 * instructions per entry, which is not enough to handle anything -- so each
 * entry does the minimum (tag itself with an index, branch to common code) and
 * the real work happens once, in C.
 *
 * The x86 side does the same thing for the same reason: isr.asm is 256 stubs
 * that push a vector number and jump to one handler. What differs is that the
 * hardware here dictates the SPACING, not just the count.
 *
 * Table layout is architectural (Arm ARM D1.10.2), four groups of four:
 *
 *   0x000  current EL, SP_EL0     ("EL1t") -- sync, irq, fiq, serror
 *   0x200  current EL, SP_ELx     ("EL1h") -- sync, irq, fiq, serror
 *   0x400  lower EL, AArch64      (EL0)    -- sync, irq, fiq, serror
 *   0x600  lower EL, AArch32               -- sync, irq, fiq, serror
 *
 * We run at EL1h, so a kernel bug lands in the 0x200 group. A syscall from
 * user space will land at 0x400 in A5. The 0x600 group can only fire if
 * something ran AArch32 code, which this kernel never does -- it is kept
 * because an entry that is "impossible" and MISSING gives you a silent jump
 * into whatever follows the table, while one that is present gives you a
 * decoded panic naming the impossible thing that just happened.
 */

/* Must match struct aarch64_frame in exception.h. Checked there by
 * _Static_assert, which fires at compile time if the two drift apart. */
    .set FRAME_SIZE, 288
    .set F_X30,      240
    .set F_SP,       248
    .set F_ELR,      256
    .set F_SPSR,     264
    .set F_ESR,      272
    .set F_FAR,      280

/* A vector slot is 128 bytes -- THIRTY-TWO INSTRUCTIONS -- and building the
 * frame takes about fifty. So the per-entry stub does only what must be done
 * per-entry (make room, save the two registers it is about to use, record
 * WHICH entry fired) and branches to code shared by all sixteen.
 *
 * Four instructions, well inside the slot. The alternative -- inlining the
 * whole save -- assembles without complaint and silently overruns into the
 * next entry, so the table would still look right and every exception but the
 * first would execute the tail of its predecessor. */
    .section .text.vectors, "ax"

.macro EXC_ENTRY index
    sub     sp, sp, #FRAME_SIZE
    stp     x0, x1, [sp, #16 * 0]
    mov     x0, #\index
    b       exc_common
.endm

exc_common:
    /* x0 = entry index, x0/x1 already saved by the stub. */
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #F_X30]

    /* Snapshot the syndrome registers into the frame. They must be read before
     * anything can take a nested exception and overwrite them, which is why
     * this happens here and not in C. */
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x2, x3, [sp, #F_ELR]
    mrs     x2, esr_el1
    mrs     x3, far_el1
    stp     x2, x3, [sp, #F_ESR]

    /* The SP the exception interrupted -- and WHICH REGISTER that is depends on
     * where the exception came from. From EL1 it is the stack we are standing
     * on, so the interrupted value is our SP plus the frame we just cut. From
     * EL0 it is SP_EL0, an ENTIRELY DIFFERENT REGISTER that the exception did
     * not touch.
     *
     * Saving it is not merely so a fault report prints the right number. SP_EL0
     * is per-thread user state and NOTHING ELSE PRESERVES IT: the scheduler
     * switches kernel contexts (kcontext.S saves x19-x28/SP_EL1/LR/DAIF), and
     * SP_EL0 is in none of them. Without this, preempting a thread at EL0,
     * running another thread of the same process, and returning leaves the
     * FIRST thread running on the SECOND one's stack -- which is what happened:
     * hello.elf's main() returned onto a joined thread's freed stack and took a
     * translation fault in its own epilogue, five instructions from a clean
     * exit. x86 never has this bug because the user RSP rides in the iretq
     * frame the hardware itself builds.
     *
     * SPSR.M[3:0] == 0 is EL0t; EL1t/EL1h are 0b0100/0b0101. x3 still holds the
     * SPSR read just above. */
    and     x2, x3, #0xF
    cbnz    x2, 1f
    mrs     x2, sp_el0
    b       2f
1:  add     x2, sp, #FRAME_SIZE
2:  str     x2, [sp, #F_SP]

    mov     x1, sp
    bl      aarch64_exception

    /* Returning is normal: exception_probe() recovers from a fault by
     * advancing ELR, and A3's timer IRQ will return here every tick. The
     * handler may have CHANGED elr/spsr in the frame, so reload them. */
    ldp     x0, x1, [sp, #F_ELR]
    msr     elr_el1, x0
    msr     spsr_el1, x1

    /* Restore SP_EL0 when we are returning to EL0 -- the other half of the save
     * above, and the reason a preempted user thread comes back to its OWN
     * stack. Reloaded from the frame rather than simply left alone, so it
     * survives having been switched away from; a handler that rewrites the
     * frame's sp redirects the user stack, exactly as rewriting elr redirects
     * execution. Skipped entirely when returning to EL1, where SP_EL0 is not
     * the stack in use and writing it would be meaningless. */
    and     x0, x1, #0xF
    cbnz    x0, 3f
    ldr     x0, [sp, #F_SP]
    msr     sp_el0, x0
3:

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #F_X30]

    add     sp, sp, #FRAME_SIZE
    eret

/* .balign 128 before each entry, not `.org`: with `.org` an oversized stub
 * silently shifts every later entry and the table still LOOKS right. */
    .balign 2048
    .globl aarch64_vectors
aarch64_vectors:

    /* --- current EL, SP_EL0 ("EL1t"). We never run on SP_EL0 at EL1, so all
     * four of these are "cannot happen" -- and are therefore exactly the ones
     * worth catching loudly. */
    .balign 128
    EXC_ENTRY 0
    .balign 128
    EXC_ENTRY 1
    .balign 128
    EXC_ENTRY 2
    .balign 128
    EXC_ENTRY 3

    /* --- current EL, SP_ELx ("EL1h"). Kernel bugs land here. */
    .balign 128
    EXC_ENTRY 4
    .balign 128
    EXC_ENTRY 5
    .balign 128
    EXC_ENTRY 6
    .balign 128
    EXC_ENTRY 7

    /* --- lower EL, AArch64 (EL0). Syscalls will land here in A5. */
    .balign 128
    EXC_ENTRY 8
    .balign 128
    EXC_ENTRY 9
    .balign 128
    EXC_ENTRY 10
    .balign 128
    EXC_ENTRY 11

    /* --- lower EL, AArch32. Unreachable by construction; see the header note. */
    .balign 128
    EXC_ENTRY 12
    .balign 128
    EXC_ENTRY 13
    .balign 128
    EXC_ENTRY 14
    .balign 128
    EXC_ENTRY 15

    .balign 128
aarch64_vectors_end:

/* Sixteen entries of 128 bytes = 2 KiB exactly, and 2 KiB aligned, because
 * VBAR_EL1 IGNORES the low 11 bits of whatever you write to it. Get either
 * wrong and exceptions dispatch to an address that is merely nearby: not a
 * crash, a wrong handler.
 *
 * Both facts are asserted in boot/linker.ld rather than here -- `.if` on a
 * difference of two labels separated by `.balign` is not an assembly-time
 * constant, but it is a link-time one. Same guarantee, one stage later. */
    .globl aarch64_vectors_end

/* Install the table. Separate from the C side because writing VBAR_EL1 needs
 * an ISB before the CPU is guaranteed to use it -- omit that and a fault taken
 * in the next few instructions may still go to the OLD vector base, which is a
 * bug that reproduces about one time in fifty. */
    .globl aarch64_set_vbar
aarch64_set_vbar:
    msr     vbar_el1, x0
    isb
    ret
