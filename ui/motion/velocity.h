#ifndef EMBLINK_UI_MOTION_VELOCITY_H
#define EMBLINK_UI_MOTION_VELOCITY_H

/* ui/motion/velocity.h -- how fast a hand was moving when it let go.
 *
 * A throw, a flick, momentum scrolling and the rubber band all start from the
 * velocity at release, and the last two samples are the worst estimate of it:
 * pointer reports are quantised to whole pixels and arrive unevenly, so one
 * late report reads as a stop and one early one as a lunge. This fits a
 * straight line through the samples of the last 80 ms (least squares) and
 * takes its slope -- what Android's VelocityTracker and UIKit's pan recogniser
 * do. A hand that held still before letting go has no samples left in the
 * window and throws nothing.
 *
 * Integers only, like spring.h, so the kernel and the toolkit share it. */

#include <stdint.h>

#define VEL_SAMPLES   16
#define VEL_WINDOW_NS 80000000ull

struct vel_track {
    uint64_t t[VEL_SAMPLES];
    int32_t  x[VEL_SAMPLES], y[VEL_SAMPLES];
    uint32_t head, n;                         /* head: where the next sample goes */
};

static inline void vel_reset(struct vel_track *v) { v->head = 0; v->n = 0; }

/* A position (px) at a time (ns, monotonic). */
static inline void vel_add(struct vel_track *v, uint64_t t_ns, int32_t x, int32_t y) {
    v->t[v->head] = t_ns; v->x[v->head] = x; v->y[v->head] = y;
    v->head = (v->head + 1) % VEL_SAMPLES;
    if (v->n < VEL_SAMPLES) v->n++;
}

/* The velocity at `now_ns` in px/s, both axes; 0 when fewer than two samples
 * fall within the window before it. */
static inline void vel_get(const struct vel_track *v, uint64_t now_ns, int32_t *vx, int32_t *vy) {
    *vx = *vy = 0;
    if (v->n < 2) return;
    uint32_t newest = (v->head + VEL_SAMPLES - 1) % VEL_SAMPLES;
    int64_t n = 0, st = 0, sx = 0, sy = 0, stt = 0, stx = 0, sty = 0;
    for (uint32_t i = 0; i < v->n; i++) {
        uint32_t k = (newest + VEL_SAMPLES - i) % VEL_SAMPLES;
        if (v->t[k] > now_ns || now_ns - v->t[k] > VEL_WINDOW_NS) break;
        /* relative to the newest sample: small numbers, no overflow */
        int64_t t = -(int64_t)((v->t[newest] - v->t[k]) / 1000u);     /* us, <= 0 */
        int64_t x = v->x[k] - v->x[newest], y = v->y[k] - v->y[newest];
        n++; st += t; sx += x; sy += y; stt += t * t; stx += t * x; sty += t * y;
    }
    int64_t den = n * stt - st * st;
    if (n < 2 || den <= 0) return;
    *vx = (int32_t)((n * stx - st * sx) * 1000000 / den);
    *vy = (int32_t)((n * sty - st * sy) * 1000000 / den);
}

#endif
