/* lockdemo.c -- witness for the futex, and for the mutex built on it.
 *
 * Threads have existed here far longer than any way for two of them to agree
 * about anything. This proves both halves of the fix:
 *
 *   THE ANSWER IS EXACT. N threads each add to one shared counter under the
 *   lock. If the lock does not hold, the total comes out LOW -- lost updates
 *   are what a broken mutex produces, not a crash -- so the exact total is the
 *   only assertion that means anything.
 *
 *   THE SLOW PATH WAS ACTUALLY TAKEN. A mutex that never blocked would pass
 *   the test above by luck on a fast machine, and the whole point of a futex
 *   is what happens when it DOES block. The kernel counts waits and wakes, and
 *   `test futex` reads them -- so this program deliberately holds the lock
 *   long enough to force contention rather than trying to be quick.
 *
 * Exit code: 0 if the total is exact, 1 if not, 2 if a thread could not start.
 */
#include "embk.h"

#define NTHREADS 4
#define NITER    2000

static embk_mutex g_lock = EMBK_MUTEX_INIT;
static volatile long g_counter;          /* deliberately NOT atomic: the lock
                                          * is what makes this safe, and if the
                                          * lock is broken this is what shows */
static volatile int  g_done;

static volatile int g_started;

static void worker(long arg) {
    /* FORCE REAL CONTENTION, rather than hoping for it.
     *
     * Thread 0 takes the lock and holds it until every other thread has
     * announced that it is running and about to try -- so the others reach the
     * futex and actually sleep. Without this the workers can simply run one
     * after another, the fast path wins every time, and the test passes while
     * proving nothing about the part that matters.
     *
     * The announce happens BEFORE the lock attempt, which is what makes the
     * handshake work: a thread that has already blocked cannot announce. */
    if (arg == 0) {
        embk_mutex_lock(&g_lock);
        __atomic_fetch_add(&g_started, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&g_started, __ATOMIC_SEQ_CST) < NTHREADS)
            embk_yield();
        /* Everyone else is running and heading for the lock. Hold a moment
         * longer so they get past the atomic exchange and into the syscall. */
        for (volatile int d = 0; d < 200000; d++) { }
        embk_mutex_unlock(&g_lock);
    } else {
        __atomic_fetch_add(&g_started, 1, __ATOMIC_SEQ_CST);
    }

    for (int i = 0; i < NITER; i++) {
        embk_mutex_lock(&g_lock);

        /* Read-modify-write with a gap in the middle. The gap is the point:
         * it widens the window in which a second thread can interleave, so a
         * lock that does not actually exclude will lose updates reliably
         * instead of only under unlucky timing. */
        long v = g_counter;
        for (volatile int d = 0; d < 40; d++) { }
        g_counter = v + 1;

        embk_mutex_unlock(&g_lock);
    }
    __atomic_fetch_add(&g_done, 1, __ATOMIC_SEQ_CST);

    /* REQUIRED. Returning from a thread entry does not end the thread -- there
     * is no return address to go back to, and it jumps to 0. Costs one line
     * and four faulting threads to learn. */
    embk_thread_exit(0);
}

/* ===========================================================================
 * PHASE 2: a BOUNDED QUEUE -- the thing that cannot be written with a mutex
 * alone.
 *
 * Two producers and two consumers over a ring of 8 slots. A consumer that
 * finds the queue empty must WAIT for an item; a producer that finds it full
 * must WAIT for space. With only a mutex, both become "unlock, spin, relock"
 * -- a poll loop that burns a core to discover nothing changed.
 *
 * The assertion is conservation: every item produced is consumed exactly once.
 * A lost wakeup does not corrupt the sum, it HANGS -- so the test finishing at
 * all is half the result, and the totals matching is the other half.
 * ======================================================================== */
#define QCAP     8
#define NPROD    2
#define NCONS    2
#define PER_PROD 500

static embk_mutex q_lock  = EMBK_MUTEX_INIT;
static embk_cond  q_ready = EMBK_COND_INIT;     /* something changed */

static long q_buf[QCAP];
static int  q_head, q_tail, q_count;
static int  q_producers_left = NPROD;

static volatile long q_produced_sum, q_consumed_sum;
static volatile int  q_consumed_n;

static void producer(long id) {
    long local = 0;
    for (int i = 0; i < PER_PROD; i++) {
        long item = id * 1000000L + i + 1;

        embk_mutex_lock(&q_lock);
        while (q_count == QCAP)               /* while, never if */
            embk_cond_wait(&q_ready, &q_lock);
        q_buf[q_tail] = item;
        q_tail = (q_tail + 1) % QCAP;
        q_count++;
        local += item;
        embk_mutex_unlock(&q_lock);

        /* BROADCAST, not signal. One condition serves two different kinds of
         * waiter here, so a signal could wake another producer when it is a
         * consumer that can make progress -- and that thread would re-check,
         * find the queue still full, and sleep again while the item sits
         * there. Waking too many costs time; waking the wrong one hangs. */
        embk_cond_broadcast(&q_ready);
    }

    embk_mutex_lock(&q_lock);
    __atomic_fetch_add(&q_produced_sum, local, __ATOMIC_SEQ_CST);
    q_producers_left--;
    embk_mutex_unlock(&q_lock);
    embk_cond_broadcast(&q_ready);            /* wake consumers to see the end */

    embk_thread_exit(0);
}

static void consumer(long id) {
    (void)id;
    long local = 0;
    int  got = 0;

    for (;;) {
        embk_mutex_lock(&q_lock);
        while (q_count == 0 && q_producers_left > 0)
            embk_cond_wait(&q_ready, &q_lock);

        if (q_count == 0) {                   /* drained AND nobody left */
            embk_mutex_unlock(&q_lock);
            break;
        }
        long item = q_buf[q_head];
        q_head = (q_head + 1) % QCAP;
        q_count--;
        local += item;
        got++;
        embk_mutex_unlock(&q_lock);

        embk_cond_broadcast(&q_ready);        /* a producer may be waiting for space */
    }

    __atomic_fetch_add(&q_consumed_sum, local, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&q_consumed_n, got, __ATOMIC_SEQ_CST);
    embk_thread_exit(0);
}

static int run_queue(void) {
    int tid[NPROD + NCONS];
    int n = 0;

    for (int i = 0; i < NCONS; i++) {
        int64_t t = embk_thread_create(consumer, i);
        if (t < 0) return 2;
        tid[n++] = (int)t;
    }
    for (int i = 0; i < NPROD; i++) {
        int64_t t = embk_thread_create(producer, i);
        if (t < 0) return 2;
        tid[n++] = (int)t;
    }
    for (int i = 0; i < n; i++)
        (void)embk_thread_join(tid[i]);

    /* CONSERVATION. Every item produced was consumed exactly once: the counts
     * agree and so do the checksums. Counts alone would miss a duplicate that
     * replaced a drop; the sum catches it. */
    if (q_consumed_n != NPROD * PER_PROD) return 3;
    if (q_consumed_sum != q_produced_sum)  return 4;
    return 0;
}

int main(void) {
    int tid[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        int64_t t = embk_thread_create(worker, i);
        if (t < 0) return 2;
        tid[i] = (int)t;
    }

    /* Join each one rather than polling a flag: the kernel already knows when
     * a thread has exited, and asking it beats spinning until it agrees. */
    for (int i = 0; i < NTHREADS; i++)
        (void)embk_thread_join(tid[i]);

    long want = (long)NTHREADS * NITER;
    if (g_counter != want) return 1;

    return run_queue();
}
