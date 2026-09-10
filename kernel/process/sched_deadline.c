#include "process/sched.h"
#include "process/process.h"
#include "drivers/timer/timer.h"

/* ==========================================================================
 * EARLIEST DEADLINE FIRST, WITH A BUDGET -- the second policy, and the reason
 * the seam was built.
 *
 * WHAT THIS IS FOR. Two things on this machine are hurt by lateness in a way a
 * person can actually perceive: the compositor, which owes a frame every
 * 16 ms, and audio, which owes a buffer before the last one drains. Neither
 * needs much CPU. Both need it ON TIME. Round-robin cannot express that: it
 * gives every thread its turn, and a turn that arrives 30 ms late is a dropped
 * frame however fair it was.
 *
 * WHY NOT PRIORITY. Priority says "before everyone else, always", which is the
 * wrong shape twice over. Two threads that both need a cadence starve each
 * other, because the higher one wins every decision including the ones it did
 * not need to win. And a thread that needs 2 ms out of every 16 gets all 16,
 * because there is no way to say "and now I am done". A deadline says WHEN,
 * so two of them interleave correctly; a budget says HOW MUCH, so neither can
 * take the whole machine.
 *
 * THE RULE, entire:
 *   - A thread with a live deadline and unspent budget beats every thread
 *     without one.
 *   - Among those, the nearest deadline wins. That is EDF, and EDF is optimal
 *     on one core: if any ordering meets every deadline, this one does.
 *   - A thread that has spent its budget for this period keeps its place in
 *     ordinary round-robin and loses nothing else. It is not throttled, not
 *     stopped, not penalised next period. It simply stops being urgent.
 *
 * That last rule is what makes sched_period() safe to expose to ring 3 at all.
 * Combined with the admission control in sched_declare_period(), a thread's
 * declaration buys it at most budget/period of a core ahead of its neighbours,
 * and the sum of every such claim is bounded below 1.
 *
 * WHAT THIS DELIBERATELY IS NOT. It is not a hard real-time scheduler and must
 * not be described as one. There is no bounded-latency interrupt path, no
 * priority inheritance on the futex, and no admission test that accounts for
 * blocking time -- so a deadline thread that waits on a lock held by a
 * budget-spent thread can still miss, and nothing here prevents it. The claim
 * this policy makes is narrower and measurable: under CPU contention, a
 * declared thread wakes closer to its deadline than round-robin manages. The
 * selftest measures exactly that and nothing more.
 *
 * EVERY FUNCTION HERE RUNS WITH g_sched_lock HELD. See sched.h.
 * ========================================================================== */

/* CPU spent by `t` including the fragment it is running right now.
 *
 * cpu_ns is only settled when a thread is switched AWAY from, so for the
 * thread that is asking to keep running -- the common case, and the one where
 * the answer decides whether it is still inside its budget -- the settled
 * value can be a whole quantum stale. On a 16 ms period a quantum of staleness
 * is most of the budget, so the live fragment is not a refinement here, it is
 * the difference between the budget existing and not. */
static uint64_t spent_ns(const struct thread *t, uint64_t now_ns) {
    uint64_t v = t->cpu_ns;
    if (t->state == PROCESS_RUNNING && t->dispatched_ns && now_ns > t->dispatched_ns)
        v += now_ns - t->dispatched_ns;
    return v;
}

/* Open a new period if the last one has closed.
 *
 * Called both from enqueue (the truthful moment: the thread just became
 * runnable, which for a periodic thread is the top of its period) and from
 * pick (for the thread that never blocked, whose period would otherwise close
 * once and stay closed, leaving it permanently over budget). Idempotent, so
 * being called from both is not a bug -- whichever gets there first rolls it.
 *
 * The deadline advances BY WHOLE PERIODS rather than being reset to
 * now + period, so a thread that is woken a little late does not drift its
 * cadence a little later every time; the phase stays anchored to where it
 * started. A thread that has been away for longer than one period has no
 * cadence left to preserve, and resyncs. */
static void roll(struct thread *t, uint64_t now_ms) {
    if (!t->period_ms) return;
    if (now_ms < t->deadline_ms) return;             /* still inside it */

    if (now_ms - t->deadline_ms >= t->period_ms) {
        t->deadline_ms = now_ms + t->period_ms;      /* was away; resync */
    } else {
        t->deadline_ms += t->period_ms;              /* keep the phase */
    }
    t->period_cpu_ns = t->cpu_ns;                    /* budget refilled */
}

static void dl_enqueue(struct thread *t) {
    if (t->period_ms) roll(t, timer_uptime_ms());
}

/* WHAT IS LEFT, AND WHY IT IS NOT A POLICY PROBLEM.
 *
 * On the measured load (x86, three boots): round-robin's worst lateness is
 * 75 / 75 / 114 ms and it misses 70 / 78 / 94 of the 150 periods entirely.
 * This policy's worst is 13 ms in all three and it misses NONE. aarch64 agrees
 * and does better -- 51-55 ms worst down to 6 ms, under both HVF and TCG.
 *
 * What remains is the MEAN: 4.6-6.0 ms on x86, 1.7-2.2 ms on aarch64, against
 * a period of 16. That is not a policy problem and no policy can fix it.
 *
 * A sleeping thread is only made runnable by wake_expired_locked(), which runs
 * inside schedule(). On a machine with more runnable work than cores, no core
 * is idle, so nothing re-enters schedule() until the next timer tick -- the
 * thread is not late because the wrong thread was picked, it is late because
 * it was not yet a CANDIDATE when the decision was made. By the time pick() is
 * asked, its answer is already right.
 *
 * Both machines tick at 100 Hz, so half a tick is 5 ms -- which is the x86
 * number and is NOT the aarch64 one. The mechanism above is read off the code
 * and is certain; "half a tick" is the x86 arithmetic working out, and the
 * aarch64 measurement says the constant depends on how much else is competing
 * (that run is inside the boot self-test, before the desktop exists). Fixing
 * the mechanism is what would settle it, and would settle both.
 *
 * The fix is in the timer, not here: arm the per-core one-shot for the next
 * sleeper's deadline on BUSY cores too, not only on the idle path that does it
 * today. That is a change to the tickless machinery and is recorded in
 * docs/TODO.md rather than guessed at from here.
 *
 * A preempt-on-wake IPI was written for this and REMOVED, because it did not
 * work: sending the nearest-deadline thread's wake to the core running the
 * least urgent thread put the mean at 5.79 ms, against 4.62 ms without it and
 * 5.96 ms without it on the next boot -- inside the run-to-run noise, and on
 * the wrong side of it as often as not. It could not have helped: the thread
 * is not READY at the moment the IPI would fire. Kept out rather than kept in
 * on the story that it ought to help. */

/* The dispatchability rules, copied from the round-robin policy rather than
 * summarised. Each one was put there by a specific crash and the reasoning is
 * in sched_rr.c; restating them in shorter words here is how one of them would
 * come to be subtly different from the other. */
static int dispatchable(struct thread *c, struct thread *current, uint32_t cpu) {
    if (c->pinned_cpu >= 0 && c->pinned_cpu != (int)cpu) return 0;
    if (c->suspended)                                    return 0;
    if (c != current && c->running_cpu >= 0)             return 0;
    return c->state == PROCESS_READY ||
           (c == current && c->state == PROCESS_RUNNING);
}

static struct thread *dl_pick(uint32_t cpu, struct thread *current) {
    /* NOTHING HAS DECLARED, which is the state of this machine almost all of
     * the time. Skip straight to the policy that decides everything else --
     * without this the scan below runs on every scheduling decision looking
     * for a thread that does not exist, which measured 11.6 us of a 97 us
     * decision and is the whole difference between the two policies when idle.
     * A default policy must not charge for a feature nobody is using. */
    if (!sched_declared_count())
        return sched_policy_roundrobin.pick(cpu, current);

    uint64_t now_ms = timer_uptime_ms();
    uint64_t now_ns = time_get_ns();

    struct thread *best = 0;

    for (int i = 0; i < MAX_THREADS; i++) {
        struct thread *t = &thread_table[i];
        if (!t->period_ms) continue;
        if (!dispatchable(t, current, cpu)) continue;

        roll(t, now_ms);

        /* Budget spent: not urgent any more. It stays a perfectly ordinary
         * candidate for the round-robin scan below -- this `continue` removes
         * its PRIVILEGE, not its turn. */
        if (spent_ns(t, now_ns) - t->period_cpu_ns >=
            (uint64_t)t->budget_ms * 1000000ull)
            continue;

        if (!best || t->deadline_ms < best->deadline_ms)
            best = t;
    }

    if (best) return best;

    /* Nobody is urgent, which is the overwhelmingly common case -- almost no
     * thread ever declares a period. Fall through to the policy that is right
     * for everything else rather than reimplementing it. */
    return sched_policy_roundrobin.pick(cpu, current);
}

/* Priority aging still runs. A deadline thread is not exempt from it and does
 * not need to be: its urgency comes from the deadline, and its priority band
 * only decides where it lands once its budget is spent -- which is exactly the
 * moment starvation protection should apply to it like anything else. */
static void dl_tick(struct thread *current) {
    sched_policy_roundrobin.tick(current);
}

const struct sched_policy sched_policy_deadline = {
    .name    = "deadline",
    .init    = 0,
    .enqueue = dl_enqueue,
    .dequeue = 0,        /* leaving is not interesting: a blocked thread is
                          * skipped by the scan, and its budget must survive
                          * the block or a thread could refill by sleeping */
    .pick    = dl_pick,
    .tick    = dl_tick,
};
