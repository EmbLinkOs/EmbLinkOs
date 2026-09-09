#ifndef _ARCH_THREAD_H_
#define _ARCH_THREAD_H_

#include <stdint.h>

/* The per-thread machine state the scheduler has to touch on every switch,
 * and nothing else. docs/ARM64.md §2.3 -- derived, like arch_irq.h, from two
 * working implementations rather than designed against one.
 *
 * kernel/process/process.c is 3,700 lines of scheduling policy that is not
 * about any particular CPU: run queues, zombies, wait queues, capabilities,
 * fair-share accounting. It reached into arch/x86_64/ for exactly four things,
 * and this header is those four. */

/* The saved register context, and the three calls that move between them.
 * Dispatched rather than declared here because struct kcontext's SHAPE is
 * machine-specific -- x86 saves rbx/rbp/r12-r15/rsp/rip/rflags, aarch64 saves
 * x19-x29/sp/pc/DAIF -- while the three functions' contract is identical. */
#if defined(__x86_64__)
#include "arch/x86_64/cpu/kcontext.h"
#elif defined(__aarch64__)
#include "arch/aarch64/cpu/kcontext.h"
#else
#error "arch_thread.h: no context-switch implementation for this architecture"
#endif

/* Install the thread pointer -- what a compiler-generated thread-local access
 * reads through. x86 writes the IA32_FS_BASE MSR; aarch64 writes TPIDR_EL0.
 * Reinstalled on every context switch, because on x86 it is a PER-CPU
 * register holding PER-THREAD state. */
void arch_tls_base_set(uint64_t base);

/* Tell the CPU which kernel stack to use when it next takes an exception from
 * user mode.
 *
 * The two machines disagree about whether this is work at all, which is
 * exactly the sort of thing a HAL invented from one side gets wrong:
 *
 *   x86_64   MUST be called on every switch. The stack pointer for a ring
 *            transition is read from TSS.rsp0, a per-CPU table, so a stale
 *            value means the next syscall from user space builds its frame on
 *            the PREVIOUS thread's kernel stack.
 *   aarch64  is a no-op, and legitimately so: SP_EL1 is a separate register
 *            from SP_EL0, so the kernel stack pointer is simply still there
 *            when an exception arrives, and the context switch restored it
 *            along with everything else.
 *
 * Callers must not skip it on the grounds that it does nothing somewhere. */
void arch_kernel_stack_set(uint64_t kstack_top);

/* This CPU's hardware identifier, for per-CPU bookkeeping and diagnostics.
 * x86 reads the local APIC ID; aarch64 reads MPIDR_EL1's affinity 0. Stable
 * for the life of the CPU, and NOT necessarily dense or zero-based. */
uint32_t arch_cpu_id(void);

/* Leave the kernel for user mode: run `entry` at the lowest privilege level on
 * `user_sp`, with a0/a1/a2 in the first three argument registers of this
 * machine's calling convention. Does not return.
 *
 * One call covers both of the scheduler's cases. A process's main thread wants
 * SysV `main(argc, argv, envp)`; a thread created by thread_create_user()
 * wants a single `void *arg` like pthread_create's. Those are the same thing
 * with two of the slots zero, and collapsing them here is what removed the
 * last named-register assembly from kernel/process/process.c.
 *
 * The machines differ in every detail and in none of the meaning:
 *
 *   x86_64   builds an interrupt-return frame -- SS, RSP, RFLAGS, CS, RIP --
 *            on the kernel stack and executes `iretq`. The segment selectors
 *            carry the privilege level, and RFLAGS must say IF=1 or user code
 *            runs uninterruptible.
 *   aarch64  writes SP_EL0, ELR_EL1 and SPSR_EL1 and executes `eret`. There
 *            are no selectors; SPSR's low bits pick EL0, and its DAIF bits
 *            play the part of RFLAGS.IF.
 *
 * Both must ensure user code starts with interrupts ENABLED, and neither may
 * leak a kernel value in a register the program can read. */
void arch_enter_user_mode(uint64_t entry, uint64_t user_sp,
                          uint64_t a0, uint64_t a1, uint64_t a2);

/* Load / store a 16-byte pattern through the FIRST vector register, for the
 * FP-context-switch self-test in kernel/process/process.c.
 *
 * The test itself -- two kthreads holding distinct patterns across real
 * preemptions and checking them byte for byte -- is the valuable part and is
 * not machine-specific at all. Only the two instructions are: `movdqa` against
 * `%xmm0` on x86, `ldr`/`str` against `q0` on aarch64. Hoisting just those two
 * lines makes a test written to validate x86's FXSAVE/FXRSTOR validate
 * aarch64's V-register save as well, which is exactly what a second
 * architecture needs and would otherwise have had to write again.
 *
 * The buffer must be 16-byte aligned: `movdqa` faults on an unaligned operand. */
void arch_fpu_pattern_load(const void *src16);
void arch_fpu_pattern_store(void *dst16);

#endif /* _ARCH_THREAD_H_ */
