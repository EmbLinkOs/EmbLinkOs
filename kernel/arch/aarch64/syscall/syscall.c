#include "arch/aarch64/irq/exception.h"
#include "include/arch_irq.h"   /* arch_irq_enable: syscalls run unmasked */
#include "process/process.h"   /* current_thread: the kill flag is checked at the exit */
#include "include/syscall_abi.h"
#include "include/kprintf.h"

/* The aarch64 half of the system call path -- docs/ARM64.md phase A5.
 *
 * This is the counterpart of kernel/arch/x86_64/syscall/syscall.c, and it is
 * the same 30 lines of substance: take the trap frame, fill in a struct
 * sysargs, call syscall_invoke(), put the result back. A4 made that possible
 * by getting the machine out of the 95 handlers; this file is the payoff.
 *
 * THE CONVENTION:  number in x8, arguments in x0..x5, result back in x0.
 *
 * x8 for the number, not x0, so all six argument registers stay free -- the
 * same choice Linux/aarch64 makes, and a small improvement on x86, where rax
 * carries the number IN and the result OUT and reading the wrong one is a real
 * bug that has happened here before (see sys_exit's comment). */

void aarch64_syscall(struct aarch64_frame *f);

void aarch64_syscall(struct aarch64_frame *f) {
    struct sysargs a;

    a.nr = f->x[8];
    for (int i = 0; i < SYSCALL_MAX_ARGS; i++)
        a.arg[i] = f->x[i];

    /* Interrupts on for the duration, for exactly the reason the x86 side
     * documents: a syscall that reaches the block layer waits on a device
     * interrupt to complete, and an exception entry arrives with PSTATE.I set.
     * Without this, any syscall that touches storage would hang forever.
     *
     * Safe because the vector epilogue restores PSTATE from SPSR_EL1 on the
     * way out, so whatever happens to DAIF in between is discarded. */
    arch_irq_enable();

    /* WITH INTERRUPTS ON, as x86's syscall_dispatch has always run (its
     * `sti`). Taking the SVC masked IRQs, and until now nothing unmasked them
     * for the length of the call -- so a syscall that waited on a disk held
     * this core's tick off for the whole wait, and a thread inside a syscall
     * was the one kind of thread the scheduler could never preempt. User
     * code always runs unmasked, so unmasking here is legitimate; the vector
     * epilogue restores the caller's PSTATE from the frame regardless. Masked
     * again before returning so the epilogue runs as it always did. */
    arch_irq_enable();

    /* IN A KERNEL PATH for the length of the call: a kill that arrives now
     * waits for the return below, where the thread holds nothing. */
    if (current_thread) current_thread->in_kernel++;
    f->x[0] = (uint64_t)syscall_invoke(&a);
    if (current_thread) {
        current_thread->in_kernel--;
        if (current_thread->killed)
            thread_die_killed();         /* never returns */
    }
    arch_irq_disable();

    arch_irq_disable();
}
