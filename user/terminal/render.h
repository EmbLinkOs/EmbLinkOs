/* render.h -- Terminal's renderer: the screen model's cells into pixels.
 *
 * Four faces (regular, bold, italic, bold italic), then a fallback chain for
 * what they lack -- DejaVu Sans Mono, DejaVu Sans -- and a box for what
 * nothing has. Box drawing, blocks, braille and Powerline separators are
 * drawn, not taken from a font (glyphs.h). Colours resolve through the
 * model's palette; bold can also brighten the eight ANSI colours.
 *
 * A row is repainted only when what it shows changed -- its cells, their
 * colours, the cursor on it, the blink phase if it blinks -- and the rows
 * repainted are the frame's damage, which Terminal hands on so only they are
 * presented. Host-tested: tests/tty/render_test.c. */
#ifndef EMBLINK_TERM_RENDER_H
#define EMBLINK_TERM_RENDER_H
#include <stdbool.h>
#include <stdint.h>
#include "vt.h"
#include "font.h"

enum { TERM_FACE_REGULAR, TERM_FACE_BOLD, TERM_FACE_ITALIC, TERM_FACE_BOLD_ITALIC };
#define TERM_FALLBACK_MAX 4
#define TERM_MASKS 256

/* What a frame shows besides the cells. */
struct term_frame {
    unsigned back;              /* rows into the scrollback */
    bool cursor;                /* draw it */
    int cursor_x, cursor_y;
    uint8_t cursor_shape;       /* DECSCUSR: 0-2 block, 3-4 underline, 5-6 bar */
    bool cursor_hollow;         /* the window does not have the keyboard: an outline */
    bool blink_on;              /* blinking text shows in this phase */
    bool bold_is_bright;        /* bold ANSI colours 0-7 draw as 8-15 */
};

struct term_mask { uint32_t cp; int w, h; uint8_t *m; };

struct term_render {
    struct font *face[4];       /* a missing bold/italic falls back to regular */
    struct font *fallback[TERM_FALLBACK_MAX]; int nfallback;
    float size;                 /* pixels */
    int cell_w, cell_h, baseline, underline, line;   /* baseline/underline: from the cell's top */
    int cols, rows, width, height;
    uint32_t *pixels;           /* width*height, BGRA, opaque */
    uint64_t *painted;          /* per row: the signature it was painted with */
    uint64_t *next;             /* this frame's signatures (scratch) */
    bool all;                   /* repaint every row next frame */
    int dmg_y0, dmg_y1;         /* the last frame's repainted pixel rows, [y0, y1) */
    bool blink_text;            /* the last frame showed blinking text: the caller keeps time */
    int rows_painted, rows_moved;   /* the last frame's work: rows drawn, rows moved by a scroll */
    struct term_mask masks[TERM_MASKS];   /* drawn glyphs, by codepoint and size */
};

/* The faces and the size; computes the cell. faces[0] is required. */
bool term_render_fonts(struct term_render *, struct font *const faces[4],
                       struct font *const *fallback, int nfallback, float size_px);
/* cols x rows cells: (re)allocates the pixels; everything repaints. */
bool term_render_resize(struct term_render *, int cols, int rows);
/* Paint what changed since the last frame. Returns whether anything did; the
 * repainted rows are dmg_y0..dmg_y1. */
bool term_render_frame(struct term_render *, const struct vt *, const struct term_frame *);
void term_render_invalidate(struct term_render *);
void term_render_free(struct term_render *);
/* Which font draws `cp` in `face`: the face, regular, the fallbacks in
 * order; NULL when none does (a box is drawn). */
struct font *term_render_font_for(const struct term_render *, uint32_t cp, int face);
#endif
