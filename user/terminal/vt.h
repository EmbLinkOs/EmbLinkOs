/* vt.h -- the screen model Terminal draws from: a streaming VT parser and the
 * grid, scrollback and modes it drives. UI-free and host-testable.
 *
 * WHAT A CELL IS. 8 bytes: a codepoint (or a cluster id, when a base character
 * carries combining marks), a style id, a width (1, 2, or 0 for the right half
 * of a wide character) and flags. The style is interned in a per-terminal
 * table: colours are kept the way the program named them -- the default, a
 * palette index, or RGB -- so a profile's palette applies to text already on
 * the screen, and the renderer resolves them (vt_rgb).
 *
 * THE SCREEN AND THE SCROLLBACK. The primary screen is rows of cells. Rows
 * that scroll off its top join the scrollback as LOGICAL lines -- a line that
 * wrapped is stored once, unwrapped -- so the scrollback re-wraps at whatever
 * width the window has, and a resize reflows the screen and the scrollback
 * alike. The scrollback is capped in bytes, not lines. The alternate screen
 * has no scrollback and is clipped, not reflowed, on resize.
 *
 * Sequences: see docs/TERMINAL.md, "Screen model". */
#ifndef EMBLINK_VT_H
#define EMBLINK_VT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VT_MAX_COLS 1000
#define VT_MAX_ROWS 500
#define VT_HISTORY_BYTES (8u << 20)      /* default scrollback cap */

/* A colour as the program named it. */
#define VT_COLOR_DEFAULT  0u
#define VT_COLOR_INDEX(i) (0x01000000u | ((uint32_t)(i) & 0xff))
#define VT_COLOR_RGB(rgb) (0x02000000u | ((uint32_t)(rgb) & 0xffffff))
#define VT_COLOR_KIND(c)  ((c) >> 24)    /* 0 default, 1 index, 2 rgb */

/* Attributes (vt_style.attrs). */
#define VT_BOLD     0x001u
#define VT_DIM      0x002u
#define VT_ITALIC   0x004u
#define VT_BLINK    0x008u
#define VT_INVERSE  0x010u
#define VT_HIDDEN   0x020u
#define VT_STRIKE   0x040u
#define VT_OVERLINE 0x080u
#define VT_RAPID    0x100u      /* rapid blink (SGR 6) */

enum vt_underline { VT_UL_NONE, VT_UL_SINGLE, VT_UL_DOUBLE, VT_UL_CURLY, VT_UL_DOTTED, VT_UL_DASHED };

struct vt_style {
    uint32_t fg, bg, ul;        /* VT_COLOR_*; ul: the underline's colour, default = fg */
    uint16_t attrs;
    uint8_t underline;          /* enum vt_underline */
    uint8_t pad;
    uint32_t link;              /* OSC 8 hyperlink id, 0 = none */
};

#define VT_CELL_CLUSTER 0x01u   /* cp is a cluster id: a base character and its marks */

struct vt_cell {
    uint32_t cp;
    uint16_t style;
    uint8_t width;              /* 1, 2, or 0 for the right half of a wide character */
    uint8_t flags;
};

struct vt_row {
    struct vt_cell *cells;      /* VT_MAX_COLS long */
    bool wrapped;               /* the text continues on the next row */
    bool pad;                   /* the wrap left the last cell empty (a wide character moved on) */
};

struct vt_cursor {
    int x, y;
    bool wrap;                  /* the last column was written: the next character wraps first */
    bool origin;                /* DECOM: rows count from the scroll region's top */
    struct vt_style pen;        /* what the next character is drawn with */
    uint8_t charset;            /* GL is G0 (0) or G1 (1) */
    bool graphics[4];           /* G0-G3 hold DEC special graphics */
};

struct vt_screen {
    struct vt_row **rows;       /* rows[0..VT_MAX_ROWS); scrolling rotates the pointers */
    struct vt_cursor cursor, saved;
    int top, bottom;            /* the scroll region */
};

/* A logical line in the scrollback. */
struct vt_hline {
    struct vt_cell *cells;
    uint32_t n;
    uint32_t rows;              /* the rows it takes at the current width */
    bool open;                  /* the row it ended with wrapped: the next row continues it */
};

/* The colours the renderer supplies and a program may change (OSC 4/10/11/12)
 * and ask about. */
struct vt_palette {
    uint32_t color[256];        /* 0xRRGGBB */
    uint32_t fg, bg, cursor;
};

enum vt_mouse_mode { VT_MOUSE_OFF, VT_MOUSE_X10, VT_MOUSE_NORMAL, VT_MOUSE_BUTTON, VT_MOUSE_ANY };
enum vt_mouse_encoding { VT_MOUSE_ENC_DEFAULT, VT_MOUSE_ENC_UTF8, VT_MOUSE_ENC_SGR, VT_MOUSE_ENC_URXVT, VT_MOUSE_ENC_SGR_PIXELS };
enum vt_mouse_event { VT_MOUSE_PRESS, VT_MOUSE_RELEASE, VT_MOUSE_MOTION };

typedef void (*vt_reply_fn)(void *context, const char *bytes, size_t length);

struct vt {
    struct vt_screen primary, alternate;
    int cols, rows;
    int row_cap;                /* cells allocated per row (>= cols) */

    /* the scrollback */
    struct vt_hline *history;   /* a ring of `history_cap` lines */
    uint32_t history_head, history_lines, history_cap;
    size_t history_bytes, history_limit;
    uint32_t history_rows;      /* rows the scrollback takes at `cols` */
    uint64_t scrolls;           /* rows that ever left the primary screen's top */

    /* interned styles, clusters, hyperlinks */
    struct vt_style *styles; uint32_t nstyles, styles_cap; uint32_t *style_hash; uint32_t style_hash_cap;
    uint32_t (*clusters)[8]; uint8_t *cluster_len; uint32_t nclusters, clusters_cap;
    char **links; uint32_t nlinks, links_cap;
    uint16_t pen_id; bool pen_dirty;   /* the cursor pen, interned */

    /* modes */
    bool alt, alt_saved, autowrap, cursor_visible, cursor_blink, application_cursor, keypad_application;
    bool bracketed_paste, insert_mode, newline_mode, reverse_video, focus_events, sync_output;
    bool alternate_scroll, inband_resize, allow_clipboard_write;
    uint8_t cursor_style;       /* DECSCUSR 0-6 */
    enum vt_mouse_mode mouse;
    enum vt_mouse_encoding mouse_encoding;
    bool *tabs;                 /* VT_MAX_COLS */
    uint64_t revision;          /* bumps on any change that can affect the picture */

    /* key settings, not terminal state: a reset keeps them */
    bool backspace_sends_bs, alt_sends_escape;

    /* colours */
    struct vt_palette base;     /* what the profile says */
    struct vt_palette over;     /* what a program set (OSC 4/10/11/12) */
    uint8_t over_set[256 / 8];  /* which palette entries are overridden */
    bool over_fg, over_bg, over_cursor;

    /* strings a program sent */
    char title[256], icon[256], directory[256];
    char title_stack[8][256]; int title_depth;
    char clipboard[4096]; size_t clipboard_len; bool clipboard_pending;  /* OSC 52 */
    unsigned bells;             /* BEL received; the renderer compares */

    /* shell integration (OSC 133): see docs/TERMINAL.md */
    unsigned prompt_serial; bool shell_ready, edit_shell; int64_t prompt_row; int last_status;
    int pixel_width, pixel_height;

    /* the parser (vt_parse.c) */
    unsigned state;
    int params[32]; uint8_t subparam[32]; unsigned nparam;
    char intermediate[2]; unsigned nintermediate;
    char private_marker;
    bool overflow;
    char string[4096]; unsigned string_length; unsigned string_kind; bool string_escape;
    uint32_t utf8, utf8_min; unsigned utf8_left;
    uint32_t last_cp;           /* REP repeats it */
    int xtsave[16][2]; unsigned nxtsave;   /* XTSAVE: DEC private modes and their values */

    vt_reply_fn reply; void *reply_context;

    /* one scrollback row, laid out for reading (vt_view_cell) */
    struct vt_cell *view_cells; uint64_t view_revision; int64_t view_row; int view_cols;
    uint32_t view_line; int64_t view_line_top;   /* the scrollback line it came from */
};

/* Lifetime. vt_init allocates; vt_free gives everything back. */
bool vt_init(struct vt *, int cols, int rows, vt_reply_fn, void *context);
void vt_free(struct vt *);
void vt_reset(struct vt *);                     /* RIS: everything but key settings and the palette */
void vt_feed(struct vt *, const void *, size_t);
bool vt_resize(struct vt *, int cols, int rows);
void vt_clear_history(struct vt *);
void vt_set_history_limit(struct vt *, size_t bytes);
void vt_set_palette(struct vt *, const struct vt_palette *);
struct vt_screen *vt_active(struct vt *);

/* Reading the picture. `back` rows into the scrollback (0 = the live screen;
 * the alternate screen has none). A cell outside the grid reads as a blank. */
const struct vt_cell *vt_view_cell(const struct vt *, int row, int col, unsigned back);
/* A whole row of the view: `cols` cells, valid until the next view read or
 * change. NULL outside the grid. */
const struct vt_cell *vt_view_row(const struct vt *, int row, unsigned back);
/* A row's id: the same number for the same text while output scrolls it up
 * into the scrollback (a selection or a search match holds rows by id). A
 * resize re-wraps the rows and ends what ids meant. */
int64_t vt_row_id(const struct vt *, int row, unsigned back);
/* The cells of the row with that id (cols long, valid until the next view
 * read or change), and whether it wraps into the next; NULL when the row is
 * no longer kept or not yet written. */
const struct vt_cell *vt_row_cells(const struct vt *, int64_t id, bool *wrapped);
/* Where row `id` is in a view `back` rows into the scrollback, or -1. */
int vt_row_in_view(const struct vt *, int64_t id, unsigned back);
bool vt_view_wrapped(const struct vt *, int row, unsigned back);
const struct vt_style *vt_style_of(const struct vt *, const struct vt_cell *);
/* The cell's text as UTF-8 (base and marks); returns the bytes written. */
size_t vt_cell_text(const struct vt *, const struct vt_cell *, char out[64]);
size_t vt_line_text(const struct vt *, int row, unsigned back, char *dst, size_t capacity);
const char *vt_link_uri(const struct vt *, uint32_t link);   /* NULL for none */
/* A colour as RGB: `fg` says which default applies to VT_COLOR_DEFAULT. */
uint32_t vt_rgb(const struct vt *, uint32_t color, bool fg);
uint32_t vt_cursor_rgb(const struct vt *);
uint32_t vt_default_color(unsigned index);      /* the built-in 256-colour palette */

/* Shell integration. */
bool vt_prompt_erase(struct vt *);
void vt_prompt_here(struct vt *);
size_t vt_size_report(const struct vt *, char out[48]);

/* Characters. */
int vt_width(uint32_t rune);
unsigned vt_encode(uint32_t rune, char out[4]);

/* Keys (vt_keys.c): xterm's PC-style encoding. Modifier bits: Shift=1,
 * Alt=2, Ctrl=4. A value below 256 is a byte the keyboard produced -- Alt puts
 * ESC in front of it when alt_sends_escape is on. The rest are keys:
 *   cursor keys, Home, End   CSI/SS3 x (application cursor), CSI 1;m x modified
 *   Insert Delete PgUp PgDn  CSI n ~, CSI n;m ~
 *   F1-F4                    SS3 P..S, CSI 1;m P..S modified
 *   F5-F12                   CSI 15 17 18 19 20 21 23 24 ~ (;m)
 *   Backspace                DEL, or ^H with backspace_sends_bs; Ctrl gives the other
 *   keypad                   its character, or SS3 p..y n M k m j o in application mode */
enum vt_key { VT_KEY_UP=256,VT_KEY_DOWN,VT_KEY_RIGHT,VT_KEY_LEFT,VT_KEY_HOME,
    VT_KEY_END,VT_KEY_DELETE,VT_KEY_INSERT,VT_KEY_PAGEUP,VT_KEY_PAGEDOWN,VT_KEY_TAB,
    VT_KEY_F1,VT_KEY_F12=VT_KEY_F1+11,VT_KEY_BACKSPACE,VT_KEY_ENTER,VT_KEY_ESCAPE,VT_KEY_MENU,
    VT_KEY_KP_0,VT_KEY_KP_9=VT_KEY_KP_0+9,VT_KEY_KP_DOT,VT_KEY_KP_ENTER,VT_KEY_KP_PLUS,
    VT_KEY_KP_MINUS,VT_KEY_KP_STAR,VT_KEY_KP_SLASH };
size_t vt_key_sequence(const struct vt *, unsigned key, unsigned mods, char out[16]);

/* Mouse reporting (vt_keys.c). `button`: 0 left, 1 middle, 2 right, 3 none
 * (motion with no button), 4/5 wheel up/down, 6/7 wheel left/right. col and row
 * are 0-based cells (px, py pixels, for SGR-pixels). Returns the bytes to send,
 * 0 when the program did not ask for this event. */
size_t vt_mouse_report(const struct vt *, enum vt_mouse_event, int button, int col, int row,
                       int px, int py, unsigned mods, char out[48]);
/* DECSET 1004: CSI I / CSI O. 0 when focus reports are off. */
size_t vt_focus_report(const struct vt *, bool focused, char out[4]);
#endif
