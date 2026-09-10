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
    return (g_counter == want) ? 0 : 1;
}
