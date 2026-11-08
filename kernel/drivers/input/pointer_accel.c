/* kernel/drivers/input/pointer_accel.c -- see pointer_accel.h. No kernel state,
 * no kernel headers: the host test compiles this file as it is. */
#include "drivers/input/pointer_accel.h"

/* THE CURVE, in counts per millisecond. Below 0.6 -- a 1000 dpi mouse moved
 * 0.6 inch a second, which is aiming -- exactly 1:1. Then 1 to 2 by 2.4, 2 to
 * 3 by 6, and 3 beyond: a sweep across a 1920 px screen is a flick, not a
 * stroke across the desk. Piecewise linear, so there is no step a hand could
 * feel. */
#define SLOW_Q8   154u      /* 0.6  */
#define MID_Q8    614u      /* 2.4  */
#define FAST_Q8   1536u     /* 6.0  */
/* A report this long after the last starts a motion: its speed is unknown,
 * and treating it as slow (gain 1) is what makes the first count of an aim
 * land where it should. */
#define GAP_US    32000u

uint32_t ptr_accel_gain(uint32_t s) {
    if (s <= SLOW_Q8) return PTR_ACCEL_ONE;
    if (s <= MID_Q8) return PTR_ACCEL_ONE + (s - SLOW_Q8) * PTR_ACCEL_ONE / (MID_Q8 - SLOW_Q8);
    if (s <= FAST_Q8) return 2 * PTR_ACCEL_ONE + (s - MID_Q8) * PTR_ACCEL_ONE / (FAST_Q8 - MID_Q8);
    return 3 * PTR_ACCEL_ONE;
}

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

static int32_t scale(int32_t d, uint32_t gain, int32_t *rem) {
    if ((d < 0 && *rem > 0) || (d > 0 && *rem < 0)) *rem = 0;   /* turned round: owe nothing */
    int32_t v = d * (int32_t)gain + *rem;
    int32_t out = v / (int32_t)PTR_ACCEL_ONE;                   /* toward zero */
    *rem = v - out * (int32_t)PTR_ACCEL_ONE;
    return out;
}

void ptr_accel_apply(struct ptr_accel *a, int32_t *dx, int32_t *dy,
                     uint64_t now_ns, uint32_t tracking_permille) {
    uint64_t gap_us = a->last_ns && now_ns > a->last_ns ? (now_ns - a->last_ns) / 1000u : GAP_US;
    a->last_ns = now_ns;
    if (!*dx && !*dy) return;
    if (gap_us > GAP_US) gap_us = GAP_US;
    if (gap_us < 1000u) gap_us = 1000u;      /* faster than 1 kHz is a burst, not a speed */
    /* the distance without a square root: the longer leg and half the other,
     * within 12% of the true length */
    int32_t ax = iabs(*dx), ay = iabs(*dy);
    uint32_t mag = (uint32_t)(ax > ay ? ax + ay / 2 : ay + ax / 2);
    uint32_t speed_q8 = (uint32_t)((uint64_t)mag * 256u * 1000u / gap_us);
    uint32_t gain = (uint32_t)((uint64_t)ptr_accel_gain(speed_q8) * tracking_permille / 1000u);
    *dx = scale(*dx, gain, &a->rem_x);
    *dy = scale(*dy, gain, &a->rem_y);
}
