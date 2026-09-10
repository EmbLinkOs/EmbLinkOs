#include "process/sched.h"
#include "process/process.h"

/* ==========================================================================
 * ROUND ROBIN WITH PRIORITY BANDS AND AGING -- the policy this kernel has
 * always had, now living somewhere it can be replaced.
 *
 * Moved here VERBATIM rather than rewritten. Every rule below was put there by
 * a specific failure, and several of the comments name the crash that taught
 * it; re-deriving them while relocating them would be the fastest way to
 * reintroduce one. The only change is that the code is now behind a vtable.
 *
 * It scans the whole thread table on every decision, which is O(MAX_THREADS)
 * per switch and needs no per-thread bookkeeping at all -- so enqueue and
 * dequeue are genuinely unnecessary here, not merely unimplemented. That is
 * also the reason to want a different policy eventually: a scan is fine at
 * this size and is the wrong shape at any other.
 * ========================================================================== */

/* One tick of priority aging. A thread that has been READY without running for
 * PRIORITY_AGE_TICKS gets one band better, so a low-priority thread behind a
 * busy high-priority one eventually runs instead of starving. */
static void rr_tick(struct thread *current) {
    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &thread_table[i];
        if (t == current || t->state != PROCESS_READY) {
            continue;
        }
        /* The per-core idle threads are pinned and are NOT starved by
         * definition -- they exist to have nothing to do. Aging one would walk
         * it up to REALTIME and let it outrank real work; "waiting" is the
         * idles' entire job. */
        if (t->pinned_cpu >= 0) {
            continue;
        }
        /* A suspended thread (EmbDBG v2 control) is frozen by intent, not
         * starved -- the same reasoning that exempts the idles above. Aging it
         * would walk its priority up to REALTIME while frozen and, on resume,
         * let it starve everything below it (the shell that issued `resume`
         * included). Freeze means freeze: no run, no aging. */
        if (t->suspended) {
            continue;
        }
        if (++t->ticks_since_scheduled >= PRIORITY_AGE_TICKS) {
            if (t->priority > PRIORITY_REALTIME) {
                t->priority--;
            }
            t->ticks_since_scheduled = 0;
        }
    }
}

/* The next thread for `cpu`: highest-priority non-empty band first (0 =
 * highest), round-robin from the slot after `current` WITHIN that band.
 * Falling through every band with nothing else runnable naturally wraps back
 * around to `current` itself. */
static struct thread *rr_pick(uint32_t cpu, struct thread *current) {
    int start = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (&thread_table[i] == current) {
            start = i;
            break;
        }
    }

    for (int band = 0; band < SCHED_PRIORITY_BANDS; band++) {
        for (int off = 1; off <= MAX_THREADS; off++) {
            struct thread *candidate = &thread_table[(start + off) % MAX_THREADS];

            /* pinned_cpu: an adopted per-core idle/shell context may only ever
             * be dispatched by its own core -- process.h calls this a liveness
             * invariant, not a preference. */
            if (candidate->pinned_cpu >= 0 && candidate->pinned_cpu != (int)cpu) {
                continue;
            }

            /* Frozen by intent: never a candidate, whatever its state. */
            if (candidate->suspended) {
                continue;
            }

            /* STILL RUNNING SOMEWHERE IS NOT DISPATCHABLE. A READY thread
             * should always have running_cpu == -1 by construction; this costs
             * one comparison and catches the case where it does not, which is
             * two cores executing on one kernel stack. */
            if (candidate != current && candidate->running_cpu >= 0) {
                continue;
            }

            /* candidate == current ADDS the self-fallback that lets the
             * wrap-around land back on ourselves when nothing else is runnable
             * in any band. It must be combined with state == PROCESS_RUNNING,
             * not used instead of a state check: `current` can be ZOMBIE here
             * (process_exit_self sets that before calling schedule), and a
             * zombie must never be re-selected -- the mechanism has already
             * given it its one final disposition. Matching on identity alone
             * let a zombie select itself, hit the "next == current" early
             * return, and resume executing past process_exit_self.
             *
             * And matching on STATE alone -- the original bug -- let this scan
             * pick some OTHER core's live RUNNING thread instead, which was a
             * real double fault bringing up AP 3 under -smp 4. On one core the
             * distinction was invisible: current was the only RUNNING thread
             * that could exist. This condition is the intersection that avoids
             * both. */
            if ((candidate->state == PROCESS_READY ||
                 (candidate == current && candidate->state == PROCESS_RUNNING))
                && candidate->priority == band) {
                return candidate;
            }
        }
    }
    return NULL;
}

const struct sched_policy sched_policy_roundrobin = {
    .name    = "round-robin",
    .init    = 0,
    .enqueue = 0,       /* a full-table scan needs no notification */
    .dequeue = 0,
    .pick    = rr_pick,
    .tick    = rr_tick,
};
