#ifndef EMBLINK_FRAMECLOCK_H
#define EMBLINK_FRAMECLOCK_H

/* kernel/gfx/frameclock.h -- which rate the compositor composes at.
 *
 * A display that can show 120 frames a second should get them while something
 * moves under a hand -- a drag, a window opening, the pointer -- because that
 * is where the difference is felt, and nothing like them while it is still or
 * while an app merely redraws. macOS calls this ProMotion. Here it is one pure
 * function: everything it knows comes in as arguments, so the host test
 * (tests/gfx/frameclock_test.c) holds the whole policy without a display.
 *
 * Rates are in millihertz, as display_mode_refresh_mhz() gives them. */
#include <stdint.h>

/* What the display can do. A FIXED display (vrr_max_mhz 0) refreshes at
 * refresh_mhz and the compositor can only skip refreshes, so its rates are
 * the refresh divided by a whole number -- 120, 60, 40, 30, 24 on a 120 Hz
 * panel -- or a frame would land between two and be shown late every other
 * time. A VRR display refreshes when told, anywhere in [vrr_min, vrr_max]. */
struct fc_display {
    uint32_t refresh_mhz;          /* 0: unknown, taken as 60 Hz */
    uint32_t vrr_min_mhz, vrr_max_mhz;
};

/* What is asking for frames. */
struct fc_inputs {
    int      motion;               /* system motion: a drag, window motion, the
                                    * pointer, a scroll -- now or within the hold */
    uint32_t request_mhz;          /* what an animating app asked for (0: nothing
                                    * asked; FC_MAX: as fast as the display goes) */
    uint32_t frame_cost_us;        /* a recent bad frame's cost (0: unknown) */
    uint32_t power_cap_mhz;        /* battery or power saver (0: none) */
};

#define FC_MAX             0xffffffffu
#define FC_DEFAULT_MHZ     60000u   /* content no one asked more for */
#define FC_UNKNOWN_MHZ     60000u   /* a display that does not say */
#define FC_MAX_DIVISOR     8u       /* a fixed display's slowest rate: refresh/8 */
/* THE GOVERNOR: a frame may cost at most this share of its period. An even
 * 60 beats a ragged 120 -- so a rate the compositor cannot keep is not used.
 * Three quarters: half (the first choice) held a 23 ms frame to 20 Hz where
 * 30 could be kept, and left a drag under emulation stepping at 50 ms. */
#define FC_BUDGET_PERMILLE 750u

/* The rate to compose at, in millihertz, and the same as a period. */
uint32_t fc_rate_mhz(const struct fc_display *d, const struct fc_inputs *in);
uint64_t fc_period_ns(uint32_t rate_mhz);

/* WHEN THE NEXT FRAME IS DUE, given when the last was due. A desktop that has
 * been still for a period composes now: the first frame after stillness is
 * the one a hand is waiting for. One in motion composes a period after the
 * last was DUE -- phase-locked, not measured from when that frame finished,
 * so the beat does not drift by each frame's cost. */
uint64_t fc_next_due(uint64_t last_due_ns, uint64_t period_ns, uint64_t now_ns);

#endif
