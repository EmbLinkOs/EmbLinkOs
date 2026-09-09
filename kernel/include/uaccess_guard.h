#ifndef _UACCESS_GUARD_H_
#define _UACCESS_GUARD_H_

#include <stdint.h>
#include "include/types.h"
#include "process/process.h"     /* current_thread, struct thread::uaccess_* */
#include "include/arch_thread.h"  /* kernel_ctx_save/restore */

/* A RECOVERY POINT for kernel code that touches user memory.
 *
 * THE HOLE THIS CLOSES. copy_from_user()/copy_to_user() have always been
 * "validate, then memcpy": access_ok() walks the page tables to prove every
 * page in the range is mapped, and then the copy runs. That is a
 * time-of-check/time-of-use race, and docs/TODO.md has said so on both
 * architectures for a long time with the same excuse -- "correct while nothing
 * can unmap a page concurrently (one CPU, no demand paging)".
 *
 * BOTH HALVES OF THAT EXCUSE ARE NOW GONE. x86 has had SMP for a while and
 * aarch64 has it since A9, so another core can unmap between the check and the
 * copy -- with a thread of the SAME process, using munmap or by exiting. The
 * window is small and entirely reachable: the result is a kernel-mode page
 * fault inside memcpy, which is an unhandled fault, which is a panic. A user
 * program should not be able to panic the kernel by racing its own threads.
 *
 * HOW IT WORKS, and why there is no assembly here. A fixup table -- the usual
 * answer, a section of (faulting-pc, recovery-pc) pairs the fault handler
 * searches -- needs per-architecture inline asm to place the labels. This
 * kernel already has something better: kernel_ctx_save()/kernel_ctx_restore()
 * are setjmp/longjmp, they exist on both architectures with identical
 * semantics, and they are exercised by every context switch. So the recovery
 * point is a saved context, and the fault handler longjmps to it.
 *
 *     if (uaccess_arm()) {          // like setjmp: 0 on the direct call
 *         memcpy(dst, src, len);    // may fault
 *         uaccess_disarm();
 *         return OK;
 *     }
 *     return -EFAULT;               // the fault handler resumed us here
 *
 * The guard is PER THREAD, not per CPU: the copy is preemptible, and a thread
 * that is rescheduled must find its own recovery point and not a neighbour's.
 *
 * WHAT IT DOES NOT DO. A partially completed copy is not undone -- the caller
 * gets -EFAULT and the destination holds however many bytes were written
 * before the fault. That is what every kernel does here, and it is why
 * copy_to_user's contract has always been "all or an error", never "nothing on
 * error".
 */

/* Arm the recovery point. Evaluates to true on the direct pass (go ahead and
 * touch user memory) and false when a fault resumed us. Must be paired with
 * uaccess_disarm() on the success path -- an armed guard left behind would
 * catch a LATER, unrelated kernel fault and silently turn a real bug into an
 * -EFAULT.
 *
 * A MACRO, AND IT HAS TO BE.
 *
 * kernel_ctx_save() is setjmp, and setjmp has one cardinal rule: you may not
 * longjmp into a function that has already RETURNED. Its frame is gone and the
 * stack it occupied has been reused. A `bool uaccess_arm(void)` that saves the
 * context and then returns breaks precisely that rule -- the save records
 * uaccess_arm's own SP, the function returns, the caller reuses that stack for
 * its own locals, and the longjmp lands in a frame whose variables now belong
 * to somebody else.
 *
 * That is not a theoretical hazard. It is what the first version of this did,
 * and the recovery faulted a SECOND time, storing through a stale local that
 * had been overwritten with the very address the test was probing.
 *
 * As a macro the context is saved in the CALLER's frame -- copy_from_user()'s
 * -- which is still on the stack when the fault arrives, because the fault
 * happens inside the memcpy it called.
 *
 * `uaccess_armed` is volatile (see struct thread): it is written before a call
 * that returns twice and read from an exception handler, so a plain bool is
 * free to be sunk past the very access it guards. The fence is the other half
 * of that -- a compiler barrier, not a hardware one, because the reader is an
 * exception handler on THIS core and what needs ordering is the compiler's
 * scheduling, not the memory system's. (`__asm__` cannot appear in the comma
 * expression a macro needs; the builtin can.) */
#define uaccess_arm()                                                        \
    (current_thread == NULL                                                  \
        ? true            /* pre-scheduler: no user memory, nothing to race */\
        : (kernel_ctx_save(&current_thread->uaccess_ctx) == 0                \
              ? (current_thread->uaccess_armed = true,                       \
                 __atomic_signal_fence(__ATOMIC_SEQ_CST),                     \
                 true)                                                       \
              : (current_thread->uaccess_armed = false, false)))

void uaccess_disarm(void);

/* Called by each architecture's fault handler when a fault happens in KERNEL
 * mode. Returns true if it resumed a guarded copy -- in which case it does not
 * return to the caller at all -- and false if the fault is unrelated and the
 * handler should carry on to its panic. */
bool uaccess_fault_recover(void);

/* How many faults the guard has caught. A non-zero count is not an error: it
 * is a user program racing its own threads, refused. It IS worth seeing,
 * because a count that climbs steadily is a program doing it deliberately. */
uint64_t uaccess_recoveries(void);

#endif /* _UACCESS_GUARD_H_ */
