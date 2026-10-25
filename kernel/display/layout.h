/* layout.h -- how several outputs make one desktop.
 *
 * Extend: the desktop is one surface, the bounding box of every enabled
 * output, and each output scans out its own rectangle of it. Mirror: every
 * enabled output shows the same rectangle at the origin. The primary output
 * carries the top bar and the dock; the others show the desktop and windows.
 *
 * Pure arithmetic over rectangles, shared by the kernel's display control,
 * the compositor (pointer bounds, window placement) and the host tests. */
#ifndef EMBLINK_DISPLAY_LAYOUT_H
#define EMBLINK_DISPLAY_LAYOUT_H
#include <stdint.h>

#define LAYOUT_MAX_OUTPUTS 4u
/* The largest desktop surface: an Intel plane scans at most 8192 pixels wide
 * (a 32 KiB stride), and 16 M pixels is 64 MiB of framebuffer. */
#define LAYOUT_MAX_W      8192u
#define LAYOUT_MAX_H      4320u
#define LAYOUT_MAX_PIXELS 16777216u

struct layout_rect { int32_t x, y; uint32_t w, h; };

struct layout_out {
    uint8_t  connected, enabled, primary;
    int32_t  x, y;
    uint32_t w, h;                 /* the mode it shows */
};

struct layout {
    unsigned count;                /* outputs, enabled or not */
    uint8_t  mirror;
    struct layout_out out[LAYOUT_MAX_OUTPUTS];
};

/* Reasons a layout is refused. */
#define LAYOUT_OK          0
#define LAYOUT_NONE        -1      /* no output enabled */
#define LAYOUT_PRIMARY     -2      /* not exactly one primary among the enabled */
#define LAYOUT_OVERLAP     -3      /* two outputs share pixels (extend) */
#define LAYOUT_DETACHED    -4      /* an output touches no other: the pointer could not reach it */
#define LAYOUT_TOO_LARGE   -5      /* beyond what can be scanned out */
#define LAYOUT_MIRROR_SIZE -6      /* mirrored outputs at different sizes */
#define LAYOUT_DISCONNECTED -7     /* an enabled output that is not connected */

/* Check a layout and return its desktop size (the bounding box, which
 * layout_normalize has put at the origin). */
int layout_validate(const struct layout *l, uint32_t *desk_w, uint32_t *desk_h);

/* Move every enabled output so the bounding box starts at (0, 0). */
void layout_normalize(struct layout *l);

/* Place output `i` (enabled, sized) to the right of everything enabled,
 * aligned with the primary's top: where a newly connected monitor goes. */
void layout_place_right(struct layout *l, unsigned i);

/* The enabled outputs' rectangles, primary first; returns how many. */
unsigned layout_rects(const struct layout *l, struct layout_rect *out, unsigned max);

/* The rectangle a point is on, or -1. */
int layout_rect_at(const struct layout_rect *r, unsigned n, int32_t x, int32_t y);

/* Move a point to the nearest pixel that is on some output. */
void layout_clamp_point(const struct layout_rect *r, unsigned n, int32_t *x, int32_t *y);

/* A window whose centre is on no output (its output went away) moves to the
 * primary (r[0]), keeping its offset where it fits. Returns 1 if it moved. */
int layout_rehome(const struct layout_rect *r, unsigned n, int32_t *x, int32_t *y, uint32_t w, uint32_t h);

#endif
