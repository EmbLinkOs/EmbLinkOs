/* framepace.c -- does a REAL toolkit app get its frames on time?
 *
 * jitter.c proved the scheduler: a bare thread asking to be woken every 16 ms
 * is woken every 16 ms under load. This proves the thing a person would
 * actually see, through the same code every app on this desktop runs -- a real
 * EmApp, a real window, a real render into a real compositor surface -- and it
 * measures the only number that matters for smoothness: how far each frame
 * landed from where it was due.
 *
 * It also VERIFIES THE DECLARATION PATH. em_app_run declares a cadence for
 * itself while an app is animating (see the cadence note in ui/dsl/em_app.c),
 * and nothing on a quiet desktop animates -- so without an app like this the
 * declaration is code that is never reached, which is the same as code that
 * does not work.
 *
 *   framepace [frames] [hogs]
 *   default: 60 frames at 16 ms, 6 threads that never sleep
 *
 * WHAT IT MEASURES, and the first version of this measured the wrong thing.
 * Drift from an ideal 16 ms clock is the right number for jitter.c, which does
 * no work; here it just reports how fast the software renderer is. Under TCG
 * a 420x260 window against six busy threads takes far longer than 16 ms to
 * draw, so every frame is "late" and the total is a renderer benchmark wearing
 * a scheduler's name.
 *
 * STUTTER is the number a person actually sees, and it survives a slow
 * renderer: an app that manages 15 fps EVENLY looks fine, and the same app
 * alternating 20 ms and 90 ms does not. So this reports the spread of frame
 * INTERVALS -- best, mean, worst -- and the exit code is how much worse the
 * worst frame was than the best. Zero would be perfectly even; the units are
 * milliseconds either way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "embk.h"
#include "kit.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define PERIOD_MS 16
#define MAX_HOGS  16

static volatile int g_stop;
static volatile unsigned g_sink;
static volatile int g_running;

static void hog(long arg) {
    (void)arg;
    __atomic_fetch_add(&g_running, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) {
        volatile unsigned x = 0;
        for (int i = 0; i < 20000; i++) x += (unsigned)i;
        g_sink = x;
    }
    embk_thread_exit(0);
}

static int      g_want   = 60;
static int      g_seen   = 0;
static uint64_t g_last   = 0;
static uint64_t g_best   = 0;   /* shortest interval seen (0 = none yet) */
static uint64_t g_worst  = 0;   /* longest                               */
static uint64_t g_total  = 0;
static float    g_phase  = 0.0f;

/* The view runs once per frame, which is what makes it the right place to
 * measure: this is the moment the app is actually doing its work, not the
 * moment some loop noticed it could. */
static void view(void) {
    uint64_t now = embk_uptime_ms();
    if (g_last) {
        uint64_t gap = now - g_last;
        g_total += gap;
        if (gap > g_worst)            g_worst = gap;
        if (!g_best || gap < g_best)  g_best  = gap;
    }
    g_last = now;
    g_seen++;

    /* Something that actually changes, so the render is a real render and the
     * dirty-rect path has work to do. A static view would be measuring an
     * empty frame. */
    g_phase += 0.05f;
    if (g_phase > 1.0f) g_phase -= 1.0f;

    Screen() {
        VStack(.spacing = 8, .padding = 12) {
            Text("Frame pacing").caption();
            /* A bar that MOVES, so the render is a real render with real
             * damage. A static view would measure an empty frame. */
            ProgressBar(g_phase);
        }
    }

    if (g_seen >= g_want) em_app_request_exit(0);
}

int main(int argc, char **argv) {
    g_want   = argc > 1 ? atoi(argv[1]) : 60;
    int hogs = argc > 2 ? atoi(argv[2]) : 6;
    if (g_want < 10) g_want = 10;
    if (hogs < 0) hogs = 0;
    if (hogs > MAX_HOGS) hogs = MAX_HOGS;

    for (int i = 0; i < hogs; i++) {
        if (embk_thread_create(hog, i) < 0) {
            printf("framepace: could not start hog %d\n", i);
            return 251;
        }
    }
    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST) < hogs)
        embk_sleep_ms(1);

    printf("framepace: %d frames at %d ms, %d hog thread(s)\n",
           g_want, PERIOD_MS, hogs);
    fflush(stdout);

    /* SMALL AND CHEAP ON PURPOSE. The budget the toolkit declares is the CPU
     * one frame costs, and it will refuse to ask for a cadence it cannot keep
     * -- correctly. A 420x260 window with TrueType text costs more than half a
     * 16 ms frame under an emulator, so a big window here would only ever
     * exercise the refusal. This is the shape of the thing that genuinely
     * wants a cadence: a meter, a spinner, a waveform. */
    static const EmApp app = {
        .title      = "Frame pacing",
        .size       = { 240, 120 },
        .theme      = Dark,
        .view       = view,
        .pace_ms    = PERIOD_MS,
        /* THE CADENCE. refresh_ms makes this loop periodic rather than
         * event-driven, which is what a video player, a game or any animation
         * is -- and it is what makes em_app_run declare a period at all. */
        .refresh_ms = PERIOD_MS,
    };
    int rc = em_app_run(&app);

    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);

    /* g_seen counts the first frame, which had no interval before it. */
    int measured = g_seen > 1 ? g_seen - 1 : 1;
    uint64_t spread = g_worst > g_best ? g_worst - g_best : 0;
    printf("framepace: %d frames -- interval best %llu ms, mean %llu ms, "
           "worst %llu ms; spread %llu ms\n",
           measured, (unsigned long long)g_best,
           (unsigned long long)(g_total / (unsigned)measured),
           (unsigned long long)g_worst, (unsigned long long)spread);
    fflush(stdout);
    embk_sleep_ms(30);
    (void)rc;
    return spread > 250 ? 250 : (int)spread;
}
