#ifndef EMBK_COMP_STATS_H
#define EMBK_COMP_STATS_H

/* What composing the desktop costs, measured by the compositor itself
 * (embk_comp_stats, the serial `perf` command, the GUI+Shift+P overlay).
 *
 * A COMPOSE is one repaint of a screen region from the window stack and its
 * presentation: everything from the first pixel drawn to the copy to the
 * scanout (or the GPU's flush). Each one is charged to the CAUSE that asked for
 * it, because "the desktop is slow" is never actionable and "an idle desktop
 * presents 60 full screens a second" is.
 *
 * Shared by the kernel and userland; plain integers so both sides agree on the
 * layout without a version dance beyond `version`. */
#include <stdint.h>

#define EMBK_COMP_STATS_VERSION 5

#define EMBK_COMP_SRC_PRESENT 0   /* an application presented new pixels        */
#define EMBK_COMP_SRC_MOVE    1   /* a window was moved, placed or resized       */
#define EMBK_COMP_SRC_POINTER 2   /* the pointer: cursor, clicks, title-bar drags */
#define EMBK_COMP_SRC_ANIM    3   /* window motion (open, park, close)           */
#define EMBK_COMP_SRC_OTHER   4   /* create, destroy, focus, workspaces, displays */
#define EMBK_COMP_SRC_HUD     5   /* the statistics overlay redrawing itself      */
#define EMBK_COMP_SRCS        6

struct embk_comp_stats {
    uint32_t version;             /* EMBK_COMP_STATS_VERSION */
    uint32_t hud;                 /* 1 while the overlay is on screen */
    uint64_t now_ns;              /* when this snapshot was taken (uptime) */
    uint64_t since_ns;            /* when the totals below started counting */

    /* Totals since `since_ns`. The overlay's own repaints are counted in the
     * per-cause arrays only, so turning it on does not change what it reports. */
    uint64_t composes, compose_ns, pixels, full_screen;
    uint64_t src_composes[EMBK_COMP_SRCS];
    uint64_t src_pixels[EMBK_COMP_SRCS];
    uint64_t src_ns[EMBK_COMP_SRCS];

    /* The last second. */
    uint32_t sec_composes;
    uint32_t sec_kpixels;

    /* The most recent composes (up to 512): how long one takes. */
    uint32_t recent, p50_us, p95_us, max_us;

    /* Pointer motion until the compose that put the cursor there. */
    uint32_t ptr_samples, ptr_p50_us, ptr_p95_us, ptr_max_us;

    uint32_t screen_w, screen_h;
    uint32_t gpu;                 /* 1 when a GPU composes the desktop */
    uint32_t reserved;

    /* FRAMES (version 2): what the frame thread composed, each once however
     * much it held. The interval is between two frames in a row -- a frame
     * after a still desktop starts a new run and is not an interval -- so
     * its spread is the beat's jitter. */
    uint64_t frames;              /* since `since_ns` */
    uint32_t sec_frames;          /* in the last second */
    uint32_t period_us;           /* the beat the frame thread keeps */
    uint32_t intervals, interval_p50_us, interval_p95_us, interval_max_us;

    /* THE RATE (version 3): what the display can do -- its refresh, or a VRR
     * range -- the rate the frame thread chose for this moment, and the frame
     * cost the governor weighed (frameclock.h). */
    uint32_t display_mhz, vrr_min_mhz, vrr_max_mhz, rate_mhz;
    uint32_t frame_cost_us, reserved2;

    /* THE CLOCK'S OWN PRECISION (version 4): how late the frame thread began
     * a frame that was due on the beat -- what the timer controls, apart from
     * how long the frame then took. And the lead: how early the kernel wakes
     * it to cover its timer's own lateness, spun out to the instant -- the CPU
     * that precision costs per frame (0 on hardware with an exact timer).
     * And the fastest rate composed at since the reset: rate_mhz is only the
     * rate now, which on a still desktop is the slow one. */
    uint32_t lates, late_p50_us, late_p95_us, late_max_us;
    uint32_t lead_us, rate_peak_mhz;

    /* THE EFFECTS TIER (version 5): 0 full, 1 reduced -- what the compositor
     * draws now; how it is chosen, 0 by what frames cost, 1 held full, 2 held
     * reduced; and how many times it has stepped since the reset. */
    uint32_t effects, effects_mode, effects_steps, reserved3;
};

/* A FRAME, as the compositor's frame thread kept it (embk_win_frame). An
 * application drawing motion waits for the next one rather than sleeping a
 * guess: its frames land on the compositor's beat, and `next_ns` is when the
 * one after is due -- the time to draw motion FOR. */
struct embk_frame_info {
    uint64_t seq;                 /* frames composed since boot */
    uint64_t frame_ns;            /* uptime (ns) when this one finished */
    uint64_t period_ns;           /* the beat */
    uint64_t next_ns;             /* when the next is due */
};

#endif
