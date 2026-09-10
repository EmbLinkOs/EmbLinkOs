/* jitter.c -- how LATE does a periodic thread actually wake up?
 *
 * The one number a compositor and an audio thread care about. Both owe work at
 * a cadence -- a frame every 16 ms, a buffer before the last one drains -- and
 * for both, being late is the failure. Not slow: late. A frame computed in
 * 2 ms and presented 30 ms after its slot is a dropped frame, and no amount of
 * throughput fixes it.
 *
 * So this measures LATENESS AND NOTHING ELSE: the difference between when a
 * period was due and when the thread actually got the CPU back. It runs the
 * periodic thread against a configurable number of threads that never sleep,
 * because on an idle machine every scheduler looks perfect and the number
 * would prove nothing.
 *
 *   jitter [hogs] [period_ms] [cycles] [work_ms]
 *   default: 6 hogs, 16 ms period, 150 cycles, 2 ms of work per period
 *
 * The default is a 60 Hz frame loop doing 2 ms of drawing on a machine with
 * more runnable work than cores, which is the situation this is about.
 *
 * Exit code is the WORST lateness in milliseconds, capped at 250 -- so a
 * caller that can only see an exit status still learns the number that
 * matters, and `test deadline` compares two runs of it.
 */
#include "embk.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_HOGS 16

static volatile int g_stop;
static volatile int g_hogs_running;

/* A thread that is always runnable and never blocks. Not a strawman: a build,
 * a decoder and a page renderer all look exactly like this to a scheduler. */
static void hog(long arg) {
    (void)arg;
    __atomic_fetch_add(&g_hogs_running, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) {
        volatile unsigned x = 0;
        for (int i = 0; i < 20000; i++) x += (unsigned)i;
    }
    embk_thread_exit(0);
}

/* Burn `ms` of CPU. Deliberately a busy loop against the clock rather than a
 * fixed iteration count: the point is to consume a REAL duration of the
 * budget, and a calibrated loop count would drift with the host. */
static void work_ms(unsigned ms) {
    uint64_t end = embk_uptime_ms() + ms;
    while (embk_uptime_ms() < end) {
        volatile unsigned x = 0;
        for (int i = 0; i < 200; i++) x += (unsigned)i;
    }
}

/* Buckets, not an average. An average hides the case that is the whole
 * problem: 149 perfect frames and one 40 ms stall is a visible hitch and a
 * beautiful mean. */
static const unsigned BUCKET[] = { 1, 2, 4, 8, 16, 32, 64 };
#define NBUCKET (int)(sizeof(BUCKET) / sizeof(BUCKET[0]))

int main(int argc, char **argv) {
    int      hogs    = argc > 1 ? atoi(argv[1]) : 6;
    unsigned period  = argc > 2 ? (unsigned)atoi(argv[2]) : 16;
    int      cycles  = argc > 3 ? atoi(argv[3]) : 150;
    unsigned wk      = argc > 4 ? (unsigned)atoi(argv[4]) : 2;

    if (hogs < 0) hogs = 0;
    if (hogs > MAX_HOGS) hogs = MAX_HOGS;
    if (period < 4) period = 4;
    if (cycles < 1) cycles = 1;

    /* Declare the cadence. Under a policy that does not implement deadlines
     * this is stored and ignored, which is exactly why the same binary can be
     * the before and the after of the comparison. */
    int decl = embk_sched_period(period, wk + 2 > period ? period : wk + 2);
    printf("jitter: %d hog thread(s), %u ms period, %d cycles, %u ms work; "
           "sched_period -> %d\n", hogs, period, cycles, wk, decl);

    for (int i = 0; i < hogs; i++) {
        if (embk_thread_create(hog, i) < 0) {
            printf("jitter: could not start hog %d\n", i);
            return 251;
        }
    }
    /* Wait until the load is REALLY running. Starting the measurement while
     * the hogs are still being created would measure an idle machine for the
     * first several periods and flatter whatever policy is installed. */
    while (__atomic_load_n(&g_hogs_running, __ATOMIC_SEQ_CST) < hogs)
        embk_sleep_ms(1);
    embk_sleep_ms(20);

    unsigned hist[NBUCKET + 1];
    for (int i = 0; i <= NBUCKET; i++) hist[i] = 0;

    uint64_t base    = embk_uptime_ms();
    uint64_t worst   = 0;
    uint64_t total   = 0;
    unsigned missed  = 0;      /* woke after the period was already over */

    for (int i = 1; i <= cycles; i++) {
        uint64_t due = base + (uint64_t)i * period;
        uint64_t now = embk_uptime_ms();
        if (now < due) embk_sleep_ms(due - now);

        uint64_t woke = embk_uptime_ms();
        uint64_t late = woke > due ? woke - due : 0;

        total += late;
        if (late > worst) worst = late;
        if (late >= period) missed++;

        int b = NBUCKET;
        for (int k = 0; k < NBUCKET; k++)
            if (late < BUCKET[k]) { b = k; break; }
        hist[b]++;

        /* The frame's own work, inside its own budget. Without this the thread
         * spends no CPU at all and the budget half of the policy is never
         * exercised. */
        work_ms(wk);
    }

    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);

    printf("jitter: lateness over %d periods of %u ms\n", cycles, period);
    for (int k = 0; k < NBUCKET; k++)
        printf("  <%2u ms : %u\n", BUCKET[k], hist[k]);
    printf("  >=%2u ms : %u\n", BUCKET[NBUCKET - 1], hist[NBUCKET]);
    printf("jitter: worst %llu ms, mean %llu.%02llu ms, %u period(s) missed "
           "entirely\n",
           (unsigned long long)worst,
           (unsigned long long)(total / (unsigned)cycles),
           (unsigned long long)((total * 100 / (unsigned)cycles) % 100),
           missed);

    /* Let the hogs notice and leave before the process tears down. */
    embk_sleep_ms(30);

    return worst > 250 ? 250 : (int)worst;
}
