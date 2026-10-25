/* display_arrange.h -- arranging several monitors, for Settings and the desktop.
 *
 * The kernel shows a layout (embk_monitor.h): where each output sits on one
 * desktop surface, in pixels. Nobody thinks of their desk in coordinates --
 * they think "the big screen is to the right of the laptop, the laptop has the
 * menu bar". So Settings edits an ARRANGEMENT: for each display, whether it is
 * in use, its resolution, and which side of the primary display it is on; and
 * this file turns that into the coordinates the kernel takes, and back.
 *
 * It refuses an arrangement for the same reasons the kernel would (it runs the
 * kernel's own display/layout.c), so Settings can say why before anything
 * changes on screen rather than report a failed request after.
 *
 * A saved arrangement belongs to a SET of monitors. It is restored only when
 * exactly those monitors are connected again, each recognised by its identity
 * (the connector, and the maker, model and size its EDID gives), and is never
 * forced onto some other combination it was not made for.
 *
 * Built into libembk.so; tests/desktop/display_arrange_test.c checks it on the
 * host. */
#ifndef EMBLINK_DISPLAY_ARRANGE_H
#define EMBLINK_DISPLAY_ARRANGE_H
#include <stdint.h>
#include "embk_monitor.h"

#define ARRANGE_RIGHT 0
#define ARRANGE_LEFT  1
#define ARRANGE_ABOVE 2
#define ARRANGE_BELOW 3

struct arrange_out {
    int      enabled, primary;
    uint32_t width, height;
    int      side;         /* ARRANGE_*: where it is, seen from the primary */
    int32_t  order;        /* along that side, nearest the primary first */
    int32_t  offset;       /* along the edge it shares: from the primary's top (left/right) or left (above/below) */
};
struct arrange {
    int      mirror;
    unsigned count;        /* the layout's outputs, connected or not */
    struct arrange_out out[EMBK_DISPLAY_MAX_OUTPUTS];
};

/* Why an arrangement cannot be shown. */
#define ARRANGE_OK            0
#define ARRANGE_NONE          -1   /* no display in use */
#define ARRANGE_PRIMARY       -2
#define ARRANGE_OVERLAP       -3
#define ARRANGE_DETACHED      -4
#define ARRANGE_TOO_LARGE     -5
#define ARRANGE_MIRROR_SIZE   -6
#define ARRANGE_DISCONNECTED  -7
#define ARRANGE_MODE          -8   /* a display asked for a size it cannot show */
#define ARRANGE_MISMATCH      -9   /* a saved arrangement made for other monitors */
#define ARRANGE_TOO_MANY      -10  /* more displays in use than the adapter can show at once */

/* What is on screen now, as an arrangement. */
void arrange_from_layout(const struct embk_display_layout *d, struct arrange *a);

/* The request that shows `a` (positions normalised so the desktop starts at
 * 0,0), or why it cannot be shown. */
int arrange_request(const struct embk_display_layout *d, const struct arrange *a, struct embk_layout_request *r);

/* Would showing `r` change anything on screen? */
int arrange_differs(const struct embk_display_layout *d, const struct embk_layout_request *r);

/* Make output `i` the primary without moving anything: the others are
 * re-described from where they are now. Returns 0, or why it cannot. */
int arrange_set_primary(const struct embk_display_layout *d, struct arrange *a, unsigned i);

/* Use output `i`, or stop using it. A display joining goes to the right of the
 * others at its preferred size (or the mirrored size); the primary leaving
 * hands the role to the first display still in use. Returns 0, or why it
 * cannot (the last display in use cannot be turned off). */
int arrange_set_enabled(const struct embk_display_layout *d, struct arrange *a, unsigned i, int enabled);

/* Switch between extending and mirroring. Mirroring puts every display in use
 * at the largest size all of them can show. Returns 0, or ARRANGE_MIRROR_SIZE
 * when they have no size in common. */
int arrange_set_mirror(const struct embk_display_layout *d, struct arrange *a, int mirror);

/* The sizes every display in use can show, largest first; returns how many. */
unsigned arrange_common_modes(const struct embk_display_layout *d, const struct arrange *a,
                              uint32_t *w, uint32_t *h, unsigned max);

/* How far the monitor that point (x, y) was on moved from layout `was` to
 * layout `now` -- the rule the compositor applies to windows, so a program
 * keeping its own idea of where its window is can apply the same one. The
 * primary is asked first (mirrored monitors cover the same pixels). 1 with
 * the distance; 0 when the point was on no monitor or its monitor is gone. */
int arrange_follow(const struct embk_display_layout *was, const struct embk_display_layout *now,
                   int32_t x, int32_t y, int32_t *dx, int32_t *dy);

/* A short sentence for an ARRANGE_* error. */
const char *arrange_error(int code);

/* ---- saving ------------------------------------------------------------- */

/* Who a monitor is: the connector it is on and its EDID identity. */
uint32_t arrange_output_id(const struct embk_output *o);

/* The set of connected monitors, as one number; 0 when none. */
uint32_t arrange_connected_set(const struct embk_display_layout *d);

/* How many outputs are connected. */
unsigned arrange_connected(const struct embk_display_layout *d);

struct display_saved {
    int      mirror, count;          /* count 0: nothing saved */
    struct { uint32_t id; int enabled, primary, width, height, x, y; } out[EMBK_DISPLAY_MAX_OUTPUTS];
};

/* Remember what is on screen now (every connected output). */
void arrange_save(const struct embk_display_layout *d, struct display_saved *s);

/* The request that brings back a saved arrangement on these monitors.
 * 1: here it is, and it differs from the screen. 0: the screen already shows
 * it. Negative: it cannot apply here (other monitors, a size that is gone). */
int arrange_saved_request(const struct embk_display_layout *d, const struct display_saved *s,
                          struct embk_layout_request *r);

#endif
