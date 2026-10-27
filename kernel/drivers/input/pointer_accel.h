#ifndef EMBLINK_POINTER_ACCEL_H
#define EMBLINK_POINTER_ACCEL_H

/* kernel/drivers/input/pointer_accel.h -- how far the pointer goes for how far
 * the hand went.
 *
 * A mouse reported 1:1 is either too slow to cross a screen or too fast to hit
 * a target, and no fixed ratio is both: a person aims slowly and travels
 * fast. So the gain follows the hand's SPEED, as every desktop's does: 1:1
 * while it moves slowly (the pixel you aim at is the pixel you get), rising
 * to three times across a fast sweep. The tracking speed scales the whole
 * curve.
 *
 * Integers only (Q8 gains), and fractional pixels are carried to the next
 * report rather than dropped, so a slow accelerated drift is not lost to
 * rounding. Pure: everything it knows comes in as arguments, and the host test
 * (tests/input/pointer_accel_test.c) holds the curve without a mouse. */
#include <stdint.h>

#define PTR_ACCEL_ONE 256u                 /* a gain of 1.0, Q8 */

struct ptr_accel {
    int32_t  rem_x, rem_y;                 /* the fraction of a pixel owed, Q8 */
    uint64_t last_ns;                      /* the previous report */
};

/* The gain (Q8) for a hand moving at `speed_q8` counts per millisecond (Q8). */
uint32_t ptr_accel_gain(uint32_t speed_q8);

/* Scale a report's (dx, dy) -- device counts, either sign convention -- by the
 * gain for its speed, at `now_ns`, times `tracking_permille` (1000: as
 * designed). */
void ptr_accel_apply(struct ptr_accel *a, int32_t *dx, int32_t *dy,
                     uint64_t now_ns, uint32_t tracking_permille);

#endif
