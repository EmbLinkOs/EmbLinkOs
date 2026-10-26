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

#define EMBK_COMP_STATS_VERSION 1

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
};

#endif
