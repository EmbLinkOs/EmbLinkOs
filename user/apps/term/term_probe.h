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
#define TERM_PROBE_VERSION 1u
#define TERM_PROBE_LINE    1024                 /* the local prompt line */
#define TERM_PROBE_TEXT    65536                /* one screen as UTF-8 */
#define TERM_PROBE_CELLS   16384                /* cols*rows of the shown screen */

/* One cell of the shown screen: its own colours, VT_DEFAULT (0xffffffff) where
 * the profile's default applies, and the VT_* attribute bits. */
struct term_probe_cell {
    uint32_t rune, fg, bg;
    uint8_t width, flags, pad[2];
};

struct term_probe {
    uint32_t magic, version, size, serial;
    int32_t cols, rows, cursor_x, cursor_y;
    uint8_t at_prompt, dead, alt, cursor_visible;
    int32_t view, shell, master, session, sessions;
    uint32_t prompt_serial, history_count, frames;
    int32_t input_len, caret;
    uint32_t text_len, primary_len, pad;
    char cwd[256];
    char input[TERM_PROBE_LINE];
    char text[TERM_PROBE_TEXT];                 /* rows joined by '\n', trailing blanks dropped */
    char primary[TERM_PROBE_TEXT];              /* the primary screen, also while the alternate one shows */
    struct term_probe_cell cells[TERM_PROBE_CELLS];
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
#endif
