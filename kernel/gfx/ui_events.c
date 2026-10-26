/* kernel/gfx/ui_events.c -- see ui_events.h. */
#include "gfx/ui_events.h"
#include "process/process.h"
#include "drivers/timer/timer.h"
#include "include/errno.h"

static volatile uint32_t g_seq;
static struct wait_queue g_wq;
static volatile int      g_ready;

void ui_events_init(void) {
    sched_timed_queue(&g_wq);
    g_ready = 1;
}

void ui_event_post(void) {
    __atomic_add_fetch(&g_seq, 1, __ATOMIC_RELEASE);
    if (!g_ready) return;
    sched_lock();
    wait_queue_wake_all(&g_wq);
    sched_unlock();
}

int ui_event_wait(uint32_t *seq, uint32_t timeout_ms) {
    uint32_t seen = *seq;
    uint64_t deadline = timer_uptime_ms() + timeout_ms;
    for (;;) {
        sched_lock();
        /* the check and the block under the same lock a post wakes under:
         * a post between them is impossible, so none is lost */
        uint32_t now_seq = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
        if (now_seq != seen) { sched_unlock(); *seq = now_seq; return 1; }
        if (!g_ready || timer_uptime_ms() >= deadline) { sched_unlock(); return 0; }
        sched_block_timeout_locked(&g_wq, deadline);   /* returns unlocked */
        struct thread *t = current_thread;
        if (t && t->proc && t->proc->cancelled) return -EMBK_ECANCELED;
    }
}
