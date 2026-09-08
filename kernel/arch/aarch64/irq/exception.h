#ifndef _AARCH64_EXCEPTION_H
#define _AARCH64_EXCEPTION_H

#include <stdint.h>

/* aarch64 exception handling -- docs/ARM64.md phase A1.
 *
 * The x86 counterpart is arch/x86_64/irq/idt.h + isr.asm: a table the CPU
 * indexes on a fault, stubs that build a frame, one C function that decodes
 * it. Same three pieces here, different hardware:
 *
 *   IDT (256 gates, one per vector)  ->  VBAR_EL1 (16 entries, 128 bytes each)
 *   error code pushed by the CPU     ->  ESR_EL1  (a decoded syndrome)
 *   CR2 for page faults              ->  FAR_EL1
 *   iretq                            ->  eret, via ELR_EL1 + SPSR_EL1
 *
 * ARM is better here and it is worth saying why: x86 tells you "#PF, error
 * code 2" and leaves you to guess. ESR_EL1 carries an exception class AND a
 * fault status code, so the handler can say "data abort, level 2 translation
 * fault, on a write" without inference. */

/* The register frame the vector stubs build on the stack. Order and size are
 * fixed by vectors.S -- keep the two in step; there is a _Static_assert in
 * exception.c that catches the size drifting, though not the order. */
struct aarch64_frame {
    uint64_t x[31];     /* x0..x30. x30 is LR; there is no separate slot.   */
    uint64_t sp;        /* SP at the point of the exception, before the
                         * stub subtracted the frame.                       */
    uint64_t elr;       /* where to resume: the faulting instruction, or the
                         * one after it for SVC/BRK.                        */
    uint64_t spsr;      /* PSTATE at the point of the exception.            */
    uint64_t esr;       /* the syndrome: WHAT happened.                     */
    uint64_t far;       /* the address, for aborts and alignment faults.    */
};

/* Which of the 16 vector-table slots fired. The table's shape is architectural
 * -- four groups of {sync, irq, fiq, serror} -- and knowing the group is half
 * the diagnosis: "IRQ from a lower EL" and "IRQ from EL1" are different bugs. */
enum {
    EXC_EL1T_SYNC = 0,  EXC_EL1T_IRQ,  EXC_EL1T_FIQ,  EXC_EL1T_SERROR,
    EXC_EL1H_SYNC = 4,  EXC_EL1H_IRQ,  EXC_EL1H_FIQ,  EXC_EL1H_SERROR,
    EXC_EL0_64_SYNC = 8,  EXC_EL0_64_IRQ,  EXC_EL0_64_FIQ,  EXC_EL0_64_SERROR,
    EXC_EL0_32_SYNC = 12, EXC_EL0_32_IRQ, EXC_EL0_32_FIQ, EXC_EL0_32_SERROR,
};

/* Point VBAR_EL1 at the table. Until this is called, ANY exception is a
 * jump to whatever VBAR happens to hold -- which on a cold `virt` is 0, and
 * looks exactly like a hang. Call it early. */
void exception_init(void);

/* Run `fn` with faults made RECOVERABLE: a synchronous exception inside it
 * advances past the offending instruction and carries on, instead of panicking.
 * Returns the number of faults that were caught and stepped over.
 *
 * This exists so A1 can DEMONSTRATE the handler rather than assert it -- a
 * fault handler that has never fired is a fault handler that does not work.
 * It is not a debugging toy: A2 needs exactly this shape to ask "is there
 * memory at this physical address?" while walking the device tree, and doing
 * it now means the mechanism is already proven when that code is written.
 *
 * `verbose` prints the full decoded dump for each caught fault -- what the A1
 * self-test wants, and emphatically not what a memory scan wants.
 *
 * Only synchronous exceptions are recoverable; an IRQ or SError arriving here
 * is not "the instruction's fault" and skipping past it would silently drop
 * innocent code. This is not a general try/catch and must never be used to
 * paper over a real fault. */
unsigned exception_probe(void (*fn)(void), int verbose);

#endif /* _AARCH64_EXCEPTION_H */
