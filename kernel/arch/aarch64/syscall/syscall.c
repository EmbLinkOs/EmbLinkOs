#include "arch/aarch64/irq/exception.h"
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

    f->x[0] = (uint64_t)syscall_invoke(&a);

    arch_irq_disable();
}

/* syscall_abi.h's one architecture hook -- the thread pointer.
 *
 * x86 writes the IA32_FS_BASE MSR; here it is a plain system register, and
 * TPIDR_EL0 is what an aarch64 compiler's thread-local access reads through.
 * (TPIDRRO_EL0 is the read-only alias user code can also see; nothing uses it
 * yet.) */
void arch_tls_base_set(uint64_t base) {
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(base));
}
