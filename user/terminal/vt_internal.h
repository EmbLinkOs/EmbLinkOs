/* vt_internal.h -- what the grid (vt_grid.c) and the parser (vt_parse.c)
 * share. Not for the renderer: vt.h is the interface. */
#ifndef EMBLINK_VT_INTERNAL_H
#define EMBLINK_VT_INTERNAL_H
#include "vt.h"

#define VT_STYLE_MAX 65535u

static inline int vt_clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline struct vt_row *vt_row(struct vt_screen *s, int y) { return s->rows[y]; }

/* the pen and blanks */
uint16_t vt_pen_id(struct vt *);                 /* the cursor's pen, interned */
uint16_t vt_blank_id(struct vt *);               /* an erased cell: the pen's background only */
uint16_t vt_intern_style(struct vt *, const struct vt_style *);
uint32_t vt_intern_link(struct vt *, const char *params_and_uri);
struct vt_cell vt_blank_cell(struct vt *);

/* the grid */
void vt_print(struct vt *, uint32_t cp);         /* a printable codepoint at the cursor */
void vt_cursor_to(struct vt *, int x, int y);    /* clamped; origin mode applied by the caller */
void vt_index(struct vt *, bool reverse);        /* LF / RI, scrolling the region */
void vt_scroll(struct vt *, int top, int bottom, int count, bool up);
void vt_erase(struct vt *, int row, int from, int to);
void vt_insert_cells(struct vt *, int count);
void vt_delete_cells(struct vt *, int count);
void vt_save_cursor(struct vt *);
void vt_restore_cursor(struct vt *);
void vt_alternate(struct vt *, bool on, bool save, bool clear);   /* clear: the alternate screen, entering or leaving */
void vt_fill_e(struct vt *);                     /* DECALN */
void vt_reply(struct vt *, const char *);
#endif
