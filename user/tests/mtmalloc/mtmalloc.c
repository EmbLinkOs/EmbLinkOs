/* user/tests/mtmalloc/mtmalloc.c -- several threads in the allocator at once.
 *
 * newlib calls __malloc_lock/__malloc_unlock around every allocation and the
 * copy in libc.a locks nothing. user/lib/syscalls.c now provides a real
 * recursive one, and this program exercises it: four threads released from a
 * barrier together, one of them allocating megabyte blocks while the others
 * churn small ones, every block's contents checked before it is freed.
 *
 * WHAT IT DOES AND DOES NOT PROVE, stated plainly because the difference
 * matters. It proves the lock WORKS: a recursive lock implemented wrongly
 * deadlocks on the allocator's first re-entry, and a process that hangs here
 * is worse than the crash this replaces. It does NOT reproduce the original
 * corruption -- run with the lock removed, this passes, and three different
 * workloads were tried before writing that down (small blocks on two threads;
 * small blocks on four; one thread on big blocks beside three on small).
 *
 * The corruption is real and was observed elsewhere: the desktop's screenshot
 * writer faulted inside _free_r the instant its last write landed, while the
 * render loop was allocating mid-frame. So the lock is here because newlib's
 * own design requires it and because that crash happened -- not because this
 * program can summon it on demand. A test that cannot fail is a guard against
 * regression, not evidence for a fix, and it should not be quoted as one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "embk.h"

#define ITERS   20000
#define SLOTS   24
#define WORKERS 4          /* the machine has four cores; use all of them */

static volatile int g_done;
static volatile int g_bad;
/* A START BARRIER. Without one the first thread can finish its whole run
 * before the last one is scheduled, and then nothing was ever concurrent --
 * which is exactly what happened to the first version of this test: it passed
 * with the lock REMOVED, because the threads never overlapped. */
static volatile int g_ready;
static volatile int g_go;

static void hammer(unsigned tag) {
    int big = (tag == 0x20);              /* exactly one worker does the big ones */
    unsigned char *slot[SLOTS];
    for (int i = 0; i < SLOTS; i++) slot[i] = 0;

    __atomic_add_fetch(&g_ready, 1, __ATOMIC_SEQ_CST);
    while (!g_go) { }                       /* everybody starts together */

    for (int n = 0; n < ITERS; n++) {
        int i = n % (big ? 3 : SLOTS);   /* fewer big blocks in flight */
        if (big && n > ITERS / 8) break; /* and fewer of them: they are slow */
        if (slot[i]) {
            /* Everything this thread wrote must still be there. */
            unsigned char want = (unsigned char)(tag + (unsigned)i);
            for (int k = 0; k < 24; k++)
                if (slot[i][k] != want) { g_bad = 1; break; }
            free(slot[i]);
            slot[i] = 0;
        }
        /* ONE WORKER ALLOCATES BIG. The crash this test exists for was a free()
         * of a three-megabyte screenshot buffer while the render loop was
         * allocating small UI strings -- large blocks come from the top of the
         * heap and make the allocator do sbrk and boundary surgery, which is
         * where two threads collide. Everybody allocating 400 bytes never goes
         * near that code. */
        size_t sz = big ? (size_t)(64 * 1024 + ((n * 9973) % (768 * 1024)))
                        : (size_t)(24 + ((n * 7 + (int)tag * 13) % 400));
        unsigned char *b = (unsigned char *)malloc(sz);
        if (!b) continue;
        memset(b, (int)(unsigned char)(tag + (unsigned)i), 24);
        slot[i] = b;
    }
    for (int i = 0; i < SLOTS; i++) if (slot[i]) free(slot[i]);
}

static void worker(long arg) {
    hammer((unsigned)arg);
    __atomic_add_fetch(&g_done, 1, __ATOMIC_SEQ_CST);
    embk_thread_exit(0);
}

int main(void) {
    for (int i = 0; i < WORKERS - 1; i++) {
        if (embk_thread_create(worker, 0x20 * (i + 1)) < 0) {
            printf("mtmalloc: could not start thread %d\n", i + 1);
            return 1;
        }
    }
    /* Wait for every worker to reach the barrier, then release them all at
     * once. This thread is the last one in. */
    while (g_ready < WORKERS - 1) embk_sleep_ms(1);
    g_go = 1;
    hammer(0x80);

    for (int spin = 0; spin < 60000 && g_done < WORKERS - 1; spin++) embk_sleep_ms(1);
    if (g_done < WORKERS - 1) {
        printf("mtmalloc: %d of %d workers never finished\n",
               WORKERS - 1 - g_done, WORKERS - 1);
        return 2;
    }
    if (g_bad) { printf("mtmalloc: a block changed under its owner\n"); return 3; }

    printf("mtmalloc: %d threads, %d malloc/free pairs each, heap intact\n",
           WORKERS, ITERS);
    return 0;
}
