/* screentext.h -- what Terminal does with the text on the screen: select it,
 * search it, find the links in it. Pure, over the screen model (vt.h), and
 * host-tested (tests/tty/screentext_test.c).
 *
 * Rows are held by id (vt_row_id), so a selection or a match stays on its
 * text while output scrolls it up into the scrollback. Text is read a
 * LOGICAL line at a time -- the rows a long line wrapped onto, joined -- so a
 * word, a URL or a match can run across a wrap. */
#ifndef EMBLINK_SCREENTEXT_H
#define EMBLINK_SCREENTEXT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vt.h"

struct term_point { int64_t row; int col; };       /* row: a vt_row_id */

/* A logical line as characters, each with the cell it is in. */
struct term_text {
    uint32_t *cp;              /* a cell's character (a cluster's base) */
    int64_t *row; int *col;    /* where each one is */
    uint8_t *width;            /* 1, or 2 for a wide character */
    uint32_t *link;            /* its OSC 8 link, 0 for none */
    int n, cap;
    int64_t first, last;       /* the rows it spans */
};
/* The logical line holding row `id`; false when the row is not kept. */
bool term_text_line(const struct vt *, int64_t id, struct term_text *out);
void term_text_free(struct term_text *);

/* --- selection --------------------------------------------------------------- */

enum term_select_mode { TERM_SELECT_CHARS, TERM_SELECT_WORDS, TERM_SELECT_LINES, TERM_SELECT_BLOCK };
struct term_selection {
    bool active;
    enum term_select_mode mode;
    struct term_point anchor, head;          /* where it began, where it is now */
    struct term_point anchor_lo, anchor_hi;  /* the word or line the anchor is in */
    struct term_point head_lo, head_hi;      /* and the one the moving end is in */
};
/* Begin at p: a click (chars, or block with Alt), a double click (the word
 * under it), a triple click (its logical line). */
void term_select_start(struct term_selection *, const struct vt *, struct term_point p, enum term_select_mode);
/* Move the moving end to p (a drag, or a Shift-click), by the mode's unit. */
void term_select_extend(struct term_selection *, const struct vt *, struct term_point p);
void term_select_all(struct term_selection *, const struct vt *);
void term_select_clear(struct term_selection *);
/* The columns selected on row `id`: [*x0, *x1); false for none. */
bool term_select_span(const struct term_selection *, const struct vt *, int64_t id, int cols, int *x0, int *x1);
/* The selection as UTF-8: a wrapped line joined, trailing blanks dropped,
 * '\n' between lines (and between a block's rows). Returns the length; the
 * text is cut at `cap`-1 bytes. */
size_t term_select_text(const struct term_selection *, const struct vt *, char *out, size_t cap);
/* The start and end in order (start <= end). */
void term_select_bounds(const struct term_selection *, struct term_point *start, struct term_point *end);

/* --- find ------------------------------------------------------------------------ */

struct term_match { struct term_point start, end; };   /* cells [start, end), end exclusive */
/* The next match of `needle` (UTF-8, ignoring ASCII case) from `from`:
 * backward finds the last one starting before it, forward the first starting
 * after it. The whole scrollback and the screen are searched. */
bool term_find(const struct vt *, const char *needle, struct term_point from, bool backward, struct term_match *out);
/* The matches on row `id`, clipped to it: up to `max` spans [x0, x1). */
int term_find_spans(const struct vt *, const char *needle, int64_t id, int *x0, int *x1, int max);

/* --- links --------------------------------------------------------------------- */

/* The link under cell p: an OSC 8 hyperlink, or a URL in the text (a scheme
 * such as https:// file:// mailto:, or www.). The URL (cut at `cap`-1) and
 * the cells it covers. */
bool term_link_at(const struct vt *, struct term_point p, char *url, size_t cap, struct term_match *where);
#endif
