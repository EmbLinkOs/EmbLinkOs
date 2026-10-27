/* kernel/drivers/input/gesture.c -- what several fingers mean (gesture.h).
 *
 * A swipe begins when the third finger comes down and is measured from the
 * centroid of the fingers at that moment; it ends when fewer than three are
 * left -- fingers leave one at a time, and the swipe is what they did
 * together. The centroid's samples feed the same velocity fit the toolkit's
 * scrolling uses, so a flick is a flick wherever it is read. */

#include "gesture.h"
#include "include/spinlock.h"
#include "../../../ui/motion/velocity.h"
#include "gfx/compositor.h"            /* compositor_input_kick: a frame to read it on */

static spinlock_t       g_lock = SPINLOCK_INIT;
static struct gesture   g_g;
static struct vel_track g_vel;
static int32_t          g_cx0, g_cy0;     /* where the centroid was when it began */
static int              g_down;           /* fingers down, the last frame */
static int              g_hold;           /* two were down, and not all have lifted since */

void gesture_contacts(const int32_t *xs, const int32_t *ys, int n, uint64_t now_ns) {
    int64_t sx = 0, sy = 0;
    for (int i = 0; i < n; i++) { sx += xs[i]; sy += ys[i]; }
    int32_t cx = n ? (int32_t)(sx / n) : 0, cy = n ? (int32_t)(sy / n) : 0;
    spin_lock(&g_lock);
    int was = g_down;
    g_down = n;
    if (n >= 2) g_hold = 1;
    else if (n == 0) g_hold = 0;
    if (n >= GESTURE_FINGERS) {
        if (!g_g.active) {                    /* the third finger: a swipe begins */
            g_g.active = 1; g_g.seq++;
            g_cx0 = cx; g_cy0 = cy;
            vel_reset(&g_vel);
        }
        /* a finger more or less changes the centroid without a hand moving:
         * measure from where it would have been */
        if (n != g_g.fingers && g_g.fingers) { g_cx0 = cx - g_g.dx; g_cy0 = cy - g_g.dy; }
        g_g.fingers = n;
        g_g.dx = cx - g_cx0; g_g.dy = cy - g_cy0;
        vel_add(&g_vel, now_ns, g_g.dx, g_g.dy);
        vel_get(&g_vel, now_ns, &g_g.vx, &g_g.vy);
    } else if (g_g.active) {                  /* fewer than three: over */
        g_g.active = 0;
        vel_get(&g_vel, now_ns, &g_g.vx, &g_g.vy);
        g_g.fingers = 0;
    }
    /* a frame to read it on: fingers moving together, or their count changing */
    int kick = g_g.active || n >= 2 || was != n;
    spin_unlock(&g_lock);
    if (kick) compositor_input_kick();
}

int gesture_take(struct gesture *out) {
    spin_lock(&g_lock);
    *out = g_g;
    spin_unlock(&g_lock);
    return out->seq != 0;
}

int gesture_holds_pointer(void) { return g_hold; }
