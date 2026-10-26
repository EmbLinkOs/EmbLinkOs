#ifndef EMBLINK_UI_MOTION_SPRING_H
#define EMBLINK_UI_MOTION_SPRING_H

/* ui/motion/spring.h -- a damped spring, in fixed point, for everything that
 * moves on the desktop.
 *
 * A duration and an easing curve say how long a motion takes; they cannot say
 * what happens when it is interrupted. A window minimising that is clicked
 * back has to turn around from where it is, at the speed it has -- an eased
 * animation can only restart, and jumps. A spring has a position and a
 * velocity and is pulled toward a target, so a new target is just a new pull:
 * motion stays continuous however often it is retargeted, and a throw hands
 * it a velocity to start from. SwiftUI and Core Animation describe springs
 * the same way, and so does this: a RESPONSE (the period of the undamped
 * oscillation, how quickly it gets there) and a DAMPING FRACTION (1 arrives
 * without overshoot, less overshoots and settles, more creeps in).
 *
 *   k = (2 pi / response)^2      c = 2 zeta sqrt(k) = 4 pi zeta / response
 *
 * INTEGERS ONLY, so the kernel (built without an FPU in its own code) and the
 * toolkit run the same header and get the same motion. Positions are Q16.16
 * pixels, velocities Q16.16 pixels per second.
 *
 * THE SAME AT ANY FRAME RATE. It integrates in fixed substeps of 1/960 s
 * (semi-implicit Euler, stable for responses above 3.3 ms), counted from the
 * spring's own origin: where it is at a given moment depends on the moment,
 * not on how many frames asked on the way -- 60 Hz, 120 Hz and a ragged
 * emulator see one trajectory. First order: it trails the exact curve by
 * about 0.6% (a lag of 2 ms at its fastest), and cannot gain energy. 960 is a common multiple of 48, 60, 120, 160,
 * 240 and 480, so those rates land on substep boundaries. */

#include <stdint.h>

#define SPRING_ONE        65536           /* 1.0 in Q16.16 */
#define SPRING_HZ         960u            /* substeps a second */
#define SPRING_PX(px)     ((int64_t)(px) * SPRING_ONE)

/* THE MOTION TOKENS (response ms, damping permille). */
#define SPRING_SNAPPY     250u, 900u      /* controls, menus: quick, barely overshoots */
#define SPRING_SMOOTH     350u, 1000u     /* windows, sheets: critically damped */
#define SPRING_BOUNCY     450u, 700u      /* the dock's launch, a drop: a visible settle */

/* A spring at rest closer than this is AT its target: 1/64 px, 2 px/s. */
#define SPRING_REST_X     (SPRING_ONE / 64)
#define SPRING_REST_V     (2 * SPRING_ONE)
/* More than this behind (a stalled machine, a suspended app) and the motion
 * would long since have ended: it is placed at rest rather than replayed. */
#define SPRING_CATCHUP_S  2u

struct spring_params {
    int64_t k, c;                           /* Q16.16: 1/s^2 and 1/s */
};

struct spring {
    int64_t  x, v, target;                  /* Q16.16 px, px/s, px */
    struct spring_params p;
    uint64_t t0_ns;                         /* the clock its substeps count from */
    uint64_t steps;                         /* substeps taken since t0_ns */
    int      moving;
};

/* Division to the nearest, symmetric about zero: a spring rounding the same
 * way on both sides of its target does not creep. `b` > 0. */
static inline int64_t spring_div(int64_t a, int64_t b) {
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

static inline struct spring_params spring_params(uint32_t response_ms, uint32_t damping_permille) {
    if (response_ms < 4) response_ms = 4;                       /* the substep's stability limit */
    int64_t w = (int64_t)411775 * 1000 / response_ms;           /* 2 pi / response, Q16.16 */
    struct spring_params p = { w * w / SPRING_ONE, 2 * w * (int64_t)damping_permille / 1000 };
    return p;
}

/* At rest at `x`. */
static inline void spring_init(struct spring *s, int64_t x, uint64_t now_ns) {
    s->x = s->target = x; s->v = 0;
    s->p = spring_params(SPRING_SMOOTH);
    s->t0_ns = now_ns; s->steps = 0; s->moving = 0;
}

static inline void spring_settle_(struct spring *s, uint64_t now_ns) {
    s->x = s->target; s->v = 0; s->moving = 0;
    s->t0_ns = now_ns; s->steps = 0;
}

/* Where it is at `now_ns`. Returns 1 while it is still moving. */
static inline int spring_step(struct spring *s, uint64_t now_ns) {
    if (!s->moving) return 0;
    if (now_ns <= s->t0_ns) return 1;
    uint64_t due = (now_ns - s->t0_ns) / 1000u * SPRING_HZ / 1000000u;   /* via us: no overflow */
    if (due - s->steps > (uint64_t)SPRING_HZ * SPRING_CATCHUP_S) { spring_settle_(s, now_ns); return 0; }
    while (s->steps < due) {
        int64_t a = -spring_div(s->p.k * (s->x - s->target), SPRING_ONE)
                    - spring_div(s->p.c * s->v, SPRING_ONE);
        s->v += spring_div(a, SPRING_HZ);
        s->x += spring_div(s->v, SPRING_HZ);
        s->steps++;
        int64_t dx = s->x - s->target;
        if (dx < SPRING_REST_X && dx > -SPRING_REST_X && s->v < SPRING_REST_V && s->v > -SPRING_REST_V) {
            spring_settle_(s, now_ns);
            return 0;
        }
    }
    return 1;
}

/* Pull toward `target` with `p` from wherever it is, at whatever speed: the
 * retarget that makes every motion interruptible. Brought up to `now_ns`
 * first, so the pull starts where the spring actually is. */
static inline void spring_to(struct spring *s, int64_t target, struct spring_params p, uint64_t now_ns) {
    spring_step(s, now_ns);
    if (!s->moving) { s->t0_ns = now_ns; s->steps = 0; }
    s->target = target; s->p = p;
    s->moving = s->x != target || s->v != 0;
}

/* Add a velocity (Q16.16 px/s): a throw, a flick. */
static inline void spring_kick(struct spring *s, int64_t v, uint64_t now_ns) {
    spring_step(s, now_ns);
    if (!s->moving) { s->t0_ns = now_ns; s->steps = 0; }
    s->v += v;
    s->moving = s->x != s->target || s->v != 0;
}

/* Put it somewhere, at rest. */
static inline void spring_set(struct spring *s, int64_t x, uint64_t now_ns) {
    s->target = x;
    spring_settle_(s, now_ns);
}

/* The position to draw, in whole pixels. */
static inline int32_t spring_px(const struct spring *s) {
    return (int32_t)spring_div(s->x, SPRING_ONE);
}

#endif
