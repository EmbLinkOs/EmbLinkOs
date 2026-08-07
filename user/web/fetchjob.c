/* user/web/fetchjob.c -- see fetchjob.h.
 *
 * Small on purpose. The concurrency in a browser should be one flag and one
 * thread, not a framework: everything hard about the fetch already lives in
 * net.c, and everything hard about drawing already lives in the toolkit.
 */
#include <string.h>
#include <stdio.h>

#include "embk.h"
#include "fetchjob.h"

enum { JOB_IDLE = 0, JOB_RUNNING, JOB_DONE };

/* `volatile` because the worker writes what the UI thread reads. Both are
 * plain aligned words on x86-64 and stores are not reordered past each other,
 * so `state` published LAST is a sufficient release: by the time the UI sees
 * JOB_DONE, the result and the buffer are complete. */
static volatile int      g_state = JOB_IDLE;
static struct vnet_result g_res;
static char             *g_buf;
static size_t            g_cap;
static char              g_url[512];
static int               g_tid = -1;
static uint64_t          g_started_ms;
static int               g_tag;

static void worker(long arg) {
    (void)arg;
    /* Copy nothing, allocate nothing, touch no UI state -- just the fetch. */
    vnet_fetch(g_url, g_buf, g_cap, &g_res);
    g_state = JOB_DONE;                  /* published last: see above */
    embk_thread_exit(0);
}

int fetchjob_start(const char *url, char *buf, size_t cap, int tag) {
    if (g_state == JOB_RUNNING) return -1;

    /* Reap the previous worker before starting another. A thread that has run
     * to completion still holds its slot until someone joins it, and a browser
     * makes one of these per navigation -- unjoined, they accumulate for the
     * life of the process. */
    if (g_tid >= 0) { embk_thread_join(g_tid); g_tid = -1; }

    memset(&g_res, 0, sizeof g_res);
    snprintf(g_url, sizeof g_url, "%s", url);
    g_buf = buf;
    g_cap = cap;
    g_started_ms = embk_uptime_ms();
    g_tag = tag;
    g_state = JOB_RUNNING;                /* set BEFORE the thread exists, so a
                                           * poll racing the spawn sees RUNNING
                                           * rather than IDLE */

    int64_t tid = embk_thread_create(worker, 0);
    if (tid < 0) {
        /* No thread? Then do it here. A frozen window beats no page at all,
         * and this is the path a machine under memory pressure takes. */
        vnet_fetch(g_url, g_buf, g_cap, &g_res);
        g_state = JOB_DONE;
        return 0;
    }
    g_tid = (int)tid;
    return 0;
}

int fetchjob_poll(int tag, struct vnet_result *out) {
    if (g_state == JOB_RUNNING) return 0;
    if (g_state != JOB_DONE)    return -1;
    if (g_tag != tag) return 0;          /* someone else's -- leave it for them */
    if (g_tid >= 0) { embk_thread_join(g_tid); g_tid = -1; }
    if (out) *out = g_res;
    g_state = JOB_IDLE;
    g_buf = 0; g_cap = 0;
    return 1;
}

int fetchjob_busy(void) { return g_state == JOB_RUNNING; }

unsigned fetchjob_elapsed_ms(void) {
    if (g_state != JOB_RUNNING) return 0;
    uint64_t now = embk_uptime_ms();
    return (unsigned)(now > g_started_ms ? now - g_started_ms : 0);
}

const char *fetchjob_url(void) { return g_state == JOB_RUNNING ? g_url : ""; }
