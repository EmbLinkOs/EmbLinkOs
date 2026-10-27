#ifndef GESTURE_H
#define GESTURE_H
/* kernel/drivers/input/gesture.h -- what several fingers mean.
 *
 * One finger is a pointer (the touch driver's own business). THREE OR MORE
 * moving together are a swipe, which belongs to the system: sideways it
 * changes space, the spaces following the fingers; up it opens the
 * overview, down closes it. The recogniser only says where the fingers have
 * gone, since they came down and how fast; the compositor decides what that
 * means, on its own frame (compositor.c, gesture_frame_locked). Fed from any
 * touch source that knows its contacts, in screen pixels. */

#include <stdint.h>

#define GESTURE_FINGERS 3

struct gesture {
    uint32_t seq;          /* bumps when a swipe begins */
    int      active;       /* fingers still down */
    int      fingers;
    int32_t  dx, dy;       /* screen px the centroid has moved since it began */
    int32_t  vx, vy;       /* px/s, the last 80 ms (ui/motion/velocity.h) */
};

/* A frame of contacts: `n` of them at (xs[i], ys[i]), screen px, at `now_ns`. */
void gesture_contacts(const int32_t *xs, const int32_t *ys, int n, uint64_t now_ns);
/* The swipe as it stands; 0 if there has never been one. */
int  gesture_take(struct gesture *out);
/* Two or more fingers have come down and not all have lifted since: the
 * pointer must not press or move for them -- nor for the last finger left,
 * which would otherwise click wherever the hand happened to leave it. */
int  gesture_holds_pointer(void);

#endif
