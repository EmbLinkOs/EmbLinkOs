/* embk_layer.h -- LAYERS: pieces of a window the compositor moves itself.
 *
 * An application paints a piece of its interface -- a menu, a toast, a
 * tooltip, a sheet -- ONCE, into a layer of its window, and tells the
 * compositor where the layer is to be, how opaque and how large, each with a
 * spring to get there. The compositor then moves it on its own frame clock, at
 * the display's rate, whatever the application is doing: the application
 * wakes again only when the layer's CONTENT changes. This is how Core
 * Animation's render server works, and why a busy app's menus still glide.
 *
 *   id = embk_layer_create(win, w, h, &va)    pixels at va: w x h, premultiplied
 *   ... paint into va ...
 *   embk_layer_set(win, ops, n, EMBK_LS_AT_PRESENT)   from where, to where
 *   embk_layer_present(win, id, 0, 0, w, h)            the ops take effect here
 *
 * A layer stands in its window's place in the stacking order, just above the
 * window's own content, and may reach outside the window's frame (a menu
 * dropping below the window). It is drawn only once presented, and only while
 * its window is. The pointer over a layer that asks for it (EMBK_LF_HIT) goes
 * to its window, at window-content coordinates -- which then may be negative,
 * or past the window's size. The layer is where the APPLICATION said it would
 * be (its target), so an app hit-tests its own picture of it.
 *
 * Shared by the kernel and userland: plain integers, sizes checked. */
#ifndef EMBK_LAYER_H
#define EMBK_LAYER_H
#include <stdint.h>

#define EMBK_LAYER_MAX_PER_WINDOW 8

/* A spring: how quickly (the period, ms) and how damped (permille, 1000
 * arrives without overshoot). {0, 0}: jump. The motion tokens of
 * ui/motion/spring.h are {250, 900} snappy, {350, 1000} smooth, {450, 700}
 * bouncy. */
struct embk_spring { uint16_t response_ms, damping_milli; };

/* what an op sets (`set`), and which of those jump rather than spring (`snap`) */
#define EMBK_LP_POS      0x01u   /* x, y on `move` */
#define EMBK_LP_OPACITY  0x02u   /* opacity on `fade` */
#define EMBK_LP_SCALE    0x04u   /* scale_milli and its anchor on `grow` */
#define EMBK_LP_LOOK     0x08u   /* radius, shadow, material, flags: no motion */

/* op */
#define EMBK_LOP_SET     0u      /* go there */
#define EMBK_LOP_DESTROY 1u      /* go there, then the layer is gone (its pixels kept
                                  * for the window's next layer that fits) */

/* flags */
#define EMBK_LF_HIT      0x01u   /* the pointer over its opaque pixels goes to the window */
#define EMBK_LF_FILL     0x02u   /* ONE COLOUR: its first pixel, drawn over mat_w x mat_h --
                                  * a dimming the size of the screen from a 1 x 1 layer */

/* material */
#define EMBK_LM_NONE     0u
#define EMBK_LM_BLUR     1u      /* what is behind it, blurred and saturated, under it */

struct embk_layer_op {
    uint32_t id;
    uint16_t set, snap;          /* EMBK_LP_* */
    uint8_t  op, opacity, radius, material;
    int32_t  x, y;               /* its top-left at full size, window-content-local */
    uint16_t scale_milli;        /* 1000 = its own size; at most 1000 */
    uint16_t anchor_x_milli, anchor_y_milli;   /* the point it grows from, 0..1000 of its size */
    int8_t   shadow_dy;
    uint8_t  shadow_blur;        /* 0: no shadow */
    uint8_t  shadow_alpha;
    uint8_t  flags;              /* EMBK_LF_* */
    /* with EMBK_LP_LOOK: where in the layer its material is -- the glass
     * panel, not the shadow painted round it -- layer pixels at full size;
     * mat_w 0: all of it. The radius rounds this box. */
    int16_t  mat_x, mat_y;
    uint16_t mat_w, mat_h;
    struct embk_spring move, fade, grow;
    uint16_t reserved;           /* 0: the size, a multiple of four, said outright */
};

/* embk_layer_set flags */
#define EMBK_LS_AT_PRESENT 0x01u /* hold the ops for the layer's next present: a new
                                  * picture and where it goes take effect together */

#define EMBK_LAYER_OPS_MAX 8     /* ops in one embk_layer_set */

#endif
