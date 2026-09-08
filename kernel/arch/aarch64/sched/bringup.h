#ifndef _AARCH64_BRINGUP_SCHED_H
#define _AARCH64_BRINGUP_SCHED_H

#include <stdint.h>
#include "include/types.h"

/* A round-robin scheduler with a lifespan -- docs/ARM64.md phase A3.
 *
 * READ THIS BEFORE USING ANY OF IT.
 *
 * This is NOT a second scheduler for EmbLinkOS. `kernel/process/process.c`
 * is the scheduler, and it is going to be the aarch64 scheduler too. It cannot
 * be compiled yet: 3,700 lines that include the compositor, the surface
 * layer, the IPC channel and pipe code, the ELF and EMBX loaders, the GDT and
 * the LAPIC. Reaching it is phases A5 and A6, not A3.
 *
 * What A3 has to prove is narrower and worth proving on its own: that the
 * GIC delivers, that the generic timer fires periodically, and that
 * kernel_ctx_switch() actually switches -- because if any of those is wrong,
 * finding out while ALSO bringing up process.c is much harder. This file is
 * the smallest thing that exercises all three, and every piece of it is an
 * interface process.c already uses (struct kcontext, kernel_ctx_switch, a
 * timer tick calling into the scheduler), so what is proven here is what
 * transfers.
 *
 * IT IS DELETED AT A5. docs/TODO.md records that. If you find yourself adding
 * priorities, sleeping, or a wait queue to it, stop -- that work belongs in
 * process.c, and duplicating it here is how a codebase ends up with two
 * schedulers and a subtle disagreement between them. */

/* Take over the current execution context as thread 0 ("boot"). After this,
 * a timer tick may switch away at any moment. */
void bringup_sched_init(void);

/* Create a thread. Returns its id, or negative on failure. The thread runs
 * with interrupts enabled; returning from `fn` parks it. */
int bringup_thread_create(const char *name, void (*fn)(void *), void *arg);

/* Called from the timer interrupt handler. Picks the next runnable thread and
 * switches to it. Safe to call before init: it does nothing. */
void bringup_sched_tick(void);

/* Stop switching at the next tick, so the caller can print a report without
 * being preempted halfway through it. Threads stay where they are. */
void bringup_sched_stop(void);

/* How many times each thread has been scheduled. The evidence that preemption
 * is real rather than merely configured. */
uint64_t bringup_thread_slices(int id);
int      bringup_thread_count(void);
void     bringup_sched_dump(void);

/* Called by the trampoline when a thread function returns. */
void kthread_exited(void);

#endif /* _AARCH64_BRINGUP_SCHED_H */
