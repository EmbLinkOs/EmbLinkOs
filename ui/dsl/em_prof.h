/* ui/dsl/em_prof.h -- what an application's frames cost it, said on request.
 *
 * The compositor measures what COMPOSING costs; this is the other half, the
 * application's own frame: building the view, laying it out, painting it,
 * handing it over. While `perf apps on` holds on the serial console
 * (embk_comp_stats.app_profile), a runtime accumulates each phase in thread CPU
 * time and says one line per RUN of frames -- the frames of one animation, from
 * the first that moved to the wait for input that ends it, or every 60 frames
 * in a long one:
 *
 *   prof: Settings frames=19 full=2 build=140 layout=90 paint=820 paint_max=3100
 *         present=150 kpx=912 wall=310
 *
 * build/layout/paint/present are average microseconds a frame; paint_max the
 * dearest paint; full how many frames repainted everything; kpx the thousands
 * of pixels handed to the compositor over the run; wall the run's length in ms.
 * Off, it costs one branch a frame. */
#ifndef EM_PROF_H
#define EM_PROF_H

#include <stdint.h>
#include <stdio.h>
#include "embk.h"

struct em_prof {
    int      on;
    uint32_t n, full;
    uint64_t build, layout, paint, present, paint_max, px;
    uint64_t t0_ms;
};

static inline uint64_t em_prof_now(const struct em_prof *p) { return p->on ? embk_thread_cpu_ns() : 0; }

/* one frame's phases, in ns of thread CPU (0 when the phase did not run) */
static inline void em_prof_frame(struct em_prof *p, uint64_t build, uint64_t layout, uint64_t paint,
                                 uint64_t present, int full, uint64_t px) {
    if (!p->on) return;
    if (!p->n) p->t0_ms = embk_uptime_ms();
    p->n++; p->full += full ? 1u : 0u;
    p->build += build; p->layout += layout; p->paint += paint; p->present += present;
    if (paint > p->paint_max) p->paint_max = paint;
    p->px += px;
}

/* say the run so far, and start a new one */
static inline void em_prof_flush(struct em_prof *p, const char *who) {
    if (!p->on || !p->n) return;
    uint64_t n = p->n;
    char b[200];
    snprintf(b, sizeof b,
             "prof: %s frames=%u full=%u build=%lu layout=%lu paint=%lu paint_max=%lu present=%lu kpx=%lu wall=%lu\n",
             who, p->n, p->full, (unsigned long)(p->build / n / 1000u), (unsigned long)(p->layout / n / 1000u),
             (unsigned long)(p->paint / n / 1000u), (unsigned long)(p->paint_max / 1000u),
             (unsigned long)(p->present / n / 1000u), (unsigned long)(p->px / 1000u),
             (unsigned long)(embk_uptime_ms() - p->t0_ms));
    embk_puts(1, b);
    p->n = p->full = 0;
    p->build = p->layout = p->paint = p->present = p->paint_max = p->px = 0;
}

/* the switch, as the runtime polls the compositor's stats */
static inline void em_prof_switch(struct em_prof *p, int on, const char *who) {
    if (p->on && !on) em_prof_flush(p, who);
    p->on = on;
}

#endif
