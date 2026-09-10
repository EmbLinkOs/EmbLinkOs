#include "process/sched.h"
#include "process/process.h"
#include "include/kstring.h"

/* The live policy. See sched.h for the contract every entry point here obeys:
 * g_sched_lock is HELD, nothing may sleep, nothing may block. */

static const struct sched_policy *g_policy = &sched_policy_roundrobin;

void sched_policy_set(const struct sched_policy *p) {
    if (!p || !p->pick)
        return;              /* a policy that cannot pick is not a policy */
    g_policy = p;
    if (p->init) p->init();
}

const struct sched_policy *sched_policy_get(void) { return g_policy; }

/* Every policy that exists, by name. A table rather than a chain of strcmp so
 * that adding a policy is one line in one place -- the same reason the vtable
 * exists at all. */
static const struct sched_policy *const g_policies[] = {
    &sched_policy_roundrobin,
    &sched_policy_deadline,
};

const struct sched_policy *sched_policy_by_name(const char *name) {
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof(g_policies) / sizeof(g_policies[0]); i++)
        if (strcmp(g_policies[i]->name, name) == 0)
            return g_policies[i];
    return 0;
}

/* Change runnability AND tell the policy, in one place.
 *
 * These exist so the two can never drift. Every site that used to write
 * `t->state = PROCESS_READY` by hand was a site a run-queue policy would have
 * had to find and edit -- which is exactly the "the seam is a lie" failure
 * sched.h warns about. Setting the state without the notification would leave
 * a queue-based policy with a runnable thread it has never heard of. */
void sched_set_ready(struct thread *t) {
    if (!t) return;
    t->state = PROCESS_READY;
    if (g_policy->enqueue) g_policy->enqueue(t);
}

void sched_set_blocked(struct thread *t) {
    if (!t) return;
    if (g_policy->dequeue) g_policy->dequeue(t);
    t->state = PROCESS_BLOCKED;
}
