/* term_probe.h -- what Terminal shows, laid out for a debugger to read.
 *
 * The acceptance tools (tools/native_terminal.py) read this one symbol through
 * GDB instead of walking the screen model, so the model is free to change
 * shape: only Terminal has to keep the probe current. The layout is fixed and
 * checked below; a field that moves bumps TERM_PROBE_VERSION, and the Python
 * reader refuses a version it does not know rather than misreading it.
 *
 * `serial` is a sequence count: odd while Terminal is rewriting the probe, even
 * when it is whole. A reader that sees an odd count, or a different count after
 * reading, reads again. */
#ifndef EMBLINK_TERM_PROBE_H
#define EMBLINK_TERM_PROBE_H
#include <stddef.h>
#include <stdint.h>

#define TERM_PROBE_MAGIC   0x504d5254u          /* "TRMP" */
#define TERM_PROBE_VERSION 4u
#define TERM_PROBE_LINE    1024                 /* the local prompt line */
#define TERM_PROBE_TEXT    65536                /* one screen as UTF-8 */
#define TERM_PROBE_CELLS   16384                /* cols*rows of the shown screen */
#define TERM_PROBE_TABS    16
#define TERM_PROBE_SELECTION 4096

/* One cell of the shown screen: its first codepoint, its colours resolved to
 * 0xRRGGBB -- TERM_PROBE_DEFAULT where the profile's default applies -- and
 * TERM_PROBE_CELL_* attribute bits. */
struct term_probe_cell {
    uint32_t rune, fg, bg;
    uint8_t width, flags, pad[2];
};
#define TERM_PROBE_DEFAULT        0xffffffffu
#define TERM_PROBE_CELL_BOLD      0x01u
#define TERM_PROBE_CELL_DIM       0x02u
#define TERM_PROBE_CELL_ITALIC    0x04u
#define TERM_PROBE_CELL_UNDERLINE 0x08u   /* any underline style */
#define TERM_PROBE_CELL_INVERSE   0x10u
#define TERM_PROBE_CELL_HIDDEN    0x20u
#define TERM_PROBE_CELL_STRIKE    0x40u
#define TERM_PROBE_CELL_BLINK     0x80u

/* flags: how the line is being edited right now. */
#define TERM_PROBE_LOCAL_EDIT   1u   /* Terminal edits the line itself (a shell without edit=shell) */
#define TERM_PROBE_EDIT_SHELL   2u   /* the prompt mark said the shell edits its own line */
#define TERM_PROBE_INBAND_SIZE  4u   /* DECSET 2048: size reports go in-band */

struct term_probe {
    uint32_t magic, version, size, serial;
    int32_t cols, rows, cursor_x, cursor_y;
    uint8_t at_prompt, dead, alt, cursor_visible;
    int32_t view, shell, master, session, sessions;
    uint32_t prompt_serial, history_count, frames;
    int32_t input_len, caret;
    uint32_t text_len, primary_len, flags;      /* TERM_PROBE_LOCAL_EDIT ... */
    char cwd[256];
    char input[TERM_PROBE_LINE];
    char text[TERM_PROBE_TEXT];                 /* rows joined by '\n', trailing blanks dropped */
    char primary[TERM_PROBE_TEXT];              /* the primary screen, also while the alternate one shows */
    struct term_probe_cell cells[TERM_PROBE_CELLS];
    /* version 3: tabs, the selection, find, the font, the link under the pointer */
    uint32_t tabs, tab;                         /* how many tabs, which one is shown */
    int32_t tab_shell[TERM_PROBE_TABS];         /* each tab's shell (its spawn handle); -1 once it has exited */
    char tab_title[TERM_PROBE_TABS][64];
    uint32_t selection_len;
    char selection[TERM_PROBE_SELECTION];       /* the selected text, as Copy puts it on the clipboard */
    uint32_t find_open, find_found;             /* the find bar is open; its text was found */
    int32_t find_row, find_col;                 /* that match on the screen, or -1 */
    char find[128];
    float font_px; int32_t cell_w, cell_h;
    uint32_t link_len;
    char link[256];                             /* the link under the pointer while GUI is held */
    int32_t origin_x, origin_y;                 /* the screen's top-left cell, in the window */
    /* version 4: the profile */
    char profile[64];                           /* its name */
    uint32_t prefs_open, see_through;           /* Preferences is open; the window shows through */
    float opacity; int32_t blur;                /* the background's opacity; the blur radius asked for */
    int32_t prefs_edit;                         /* the colour Preferences is editing (0-15, then text, bold,
                                                 * background, selection, cursor), or -1 */
};

_Static_assert(sizeof(struct term_probe_cell) == 16, "probe cell layout");
_Static_assert(offsetof(struct term_probe, cols) == 16, "probe layout");
_Static_assert(offsetof(struct term_probe, at_prompt) == 32, "probe layout");
_Static_assert(offsetof(struct term_probe, view) == 36, "probe layout");
_Static_assert(offsetof(struct term_probe, prompt_serial) == 56, "probe layout");
_Static_assert(offsetof(struct term_probe, input_len) == 68, "probe layout");
_Static_assert(offsetof(struct term_probe, text_len) == 76, "probe layout");
_Static_assert(offsetof(struct term_probe, cwd) == 88, "probe layout");
_Static_assert(offsetof(struct term_probe, input) == 344, "probe layout");
_Static_assert(offsetof(struct term_probe, text) == 1368, "probe layout");
_Static_assert(offsetof(struct term_probe, primary) == 66904, "probe layout");
_Static_assert(offsetof(struct term_probe, cells) == 132440, "probe layout");
_Static_assert(offsetof(struct term_probe, tabs) == 394584, "probe layout");
_Static_assert(offsetof(struct term_probe, tab_title) == 394656, "probe layout");
_Static_assert(offsetof(struct term_probe, selection_len) == 395680, "probe layout");
_Static_assert(offsetof(struct term_probe, find_open) == 399780, "probe layout");
_Static_assert(offsetof(struct term_probe, find) == 399796, "probe layout");
_Static_assert(offsetof(struct term_probe, font_px) == 399924, "probe layout");
_Static_assert(offsetof(struct term_probe, link) == 399940, "probe layout");
_Static_assert(offsetof(struct term_probe, origin_x) == 400196, "probe layout");
_Static_assert(offsetof(struct term_probe, profile) == 400204, "probe layout");
_Static_assert(offsetof(struct term_probe, opacity) == 400276, "probe layout");
_Static_assert(sizeof(struct term_probe) == 400288, "probe layout");
#endif
