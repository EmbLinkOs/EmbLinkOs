#ifndef _SCHED_H_
#define _SCHED_H_

#include <stdint.h>
#include "include/types.h"

struct thread;

/* ==========================================================================
 * THE SCHEDULER POLICY SEAM.
 *
 * process.c owns the MECHANISM of switching threads: the lock discipline, the
 * context switch, the address-space swap, zombie hand-off, wait queues. None
 * of that has an opinion about WHICH thread should run next -- and until now
 * the opinion was welded into the middle of it, a priority-band scan inlined
 * in schedule_locked() surrounded by three hundred lines of comments about
 * double faults.
 *
 * That made the policy untouchable in practice. Anything you might want --
 * per-core run queues, a deadline scheduler for audio and the compositor, a
 * fair-share policy -- meant editing the most dangerous function in the
 * kernel, the one where two separate real bugs have already been found. This
 * splits the two apart so a new policy is a NEW FILE, and the mechanism it
 * plugs into is not edited at all.
 *
 * ----------------------------------------------------------------------
 * THE CONTRACT, and it is not negotiable: EVERY function here is called
 * WITH g_sched_lock HELD. A policy must not take it, must not sleep, must not
 * block, and must not call anything that does. It runs inside the critical
 * section that is about to context-switch this core, on the outgoing thread's
 * stack, with interrupts off.
 * ----------------------------------------------------------------------
 *
 * WHY enqueue/dequeue EXIST WHEN THE DEFAULT POLICY IGNORES THEM. The
 * round-robin policy scans the whole thread table, so it needs no notification
 * that a thread became runnable. A run-queue policy needs exactly that, and if
 * the call sites did not exist the seam would be a lie -- writing the second
 * policy would still mean editing process.c, which is the thing this exists to
 * avoid. They are hooks the mechanism calls at the truthful moments, whether
 * or not today's policy cares.
 */
struct sched_policy {
    const char *name;

    /* Once, at boot, before any thread is picked. */
    void (*init)(void);

    /* `t` just became runnable / just stopped being runnable. Called for every
     * transition, including the outgoing thread's own demotion to READY inside
     * a switch. May be NULL. */
    void (*enqueue)(struct thread *t);
    void (*dequeue)(struct thread *t);

    /* THE decision: which thread should `cpu` run next?
     *
     * `current` is what it is running now, and may legitimately be returned --
     * "keep going" is a valid answer, and is how a core with nothing else to
     * do avoids a pointless switch. Returning NULL means "nothing at all",
     * which the mechanism handles by staying where it is.
     *
     * A policy MUST NOT return a thread that is pinned to another core, is
     * suspended, or has running_cpu >= 0 (it is live on another core). Those
     * are correctness constraints of the mechanism, not preferences -- the
     * last one in particular is two cores on one kernel stack. */
    struct thread *(*pick)(uint32_t cpu, struct thread *current);

    /* One scheduler tick has passed on this core. Where a policy ages
     * priorities, charges timeslices, or decides nothing at all. May be NULL. */
    void (*tick)(struct thread *current);
};

/* The live policy. Set once at boot; there is deliberately no way to swap it
 * while threads are running -- a policy change mid-flight would strand
 * whatever state the outgoing one held about threads that are already
 * queued. */
void                       sched_policy_set(const struct sched_policy *p);
const struct sched_policy *sched_policy_get(void);

/* The policies that exist. Adding one means adding a file and a name here --
 * and nothing in process.c. */
extern const struct sched_policy sched_policy_roundrobin;

/* Mechanism-side helpers: change a thread's runnability AND tell the policy,
 * in one place, so the two can never drift apart. Both require g_sched_lock. */
void sched_set_ready(struct thread *t);
void sched_set_blocked(struct thread *t);

#endif /* _SCHED_H_ */
