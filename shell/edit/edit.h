/* ==========================================================================
 * edit.h -- the shell's line editor.
 *
 * THE SHELL OWNS ITS LINE. Until this existed, the Terminal app edited the
 * command line itself and handed the shell a finished string, so the shell
 * could not draw its own prompt, complete against its own commands, keep its
 * own history across sessions or show anything while you type. A real
 * terminal is a dumb screen; the editing belongs here, as readline is bash's
 * and zle is zsh's.
 *
 * PURE: bytes from the terminal go in (edit_feed), bytes for the terminal come
 * out through the host's write(). No file descriptors, no termios, no
 * syscalls -- the shell's main loop owns those -- so the host tests feed it
 * scripted keystrokes and run its output through the real screen model.
 *
 * TWO SCREENS:
 *   EDIT_VT    a terminal that speaks ANSI (the Terminal app): full redraws
 *              with relative cursor motion, wrapping, a completion menu, an
 *              incremental-search line, bracketed paste, in-band resize.
 *   EDIT_DUMB  the kernel's framebuffer console, which parses no escapes at
 *              all: the line is edited with backspaces and rewrites, and the
 *              navigation keys arrive as the kernel's private bytes.
 *
 * Keys (emacs, as readline and zle bind them): see docs/SHELL.md, "Editing".
 * ========================================================================== */
#ifndef __EMBK_EDIT_H__
#define __EMBK_EDIT_H__

#include <stddef.h>
#include <stdbool.h>

/* One completion candidate. `insert` replaces the word being completed (the
 * completer quotes it if it must be); `show` is what the menu lists. A
 * directory's insert ends in '/', and no space is added after it. */
struct edit_item { char *insert, *show; bool dir; };
struct edit_matches {
    size_t start;              /* byte offset where the completed word begins */
    size_t n, cap;
    struct edit_item *items;   /* malloc'd strings; the editor frees them */
};
/* For a completer: append one candidate (both strings are copied). */
bool edit_matches_add(struct edit_matches *m, const char *insert, const char *show, bool dir);

struct edit_host {
    void (*write)(void *ctx, const char *bytes, size_t n);
    /* Candidates for the word ending at `cursor`. NULL = no completion. */
    void (*complete)(void *ctx, const char *line, size_t cursor, struct edit_matches *out);
    /* True when `text` is an unfinished program (an open brace, a trailing
     * pipe, an unterminated string): Enter then starts another line instead
     * of running it. NULL = every line is complete. */
    bool (*incomplete)(void *ctx, const char *text);
    void *ctx;
};

enum edit_mode { EDIT_VT, EDIT_DUMB };
enum edit_result {
    EDIT_MORE,      /* still editing: feed more bytes */
    EDIT_LINE,      /* Enter on a complete program: edit_line() has it */
    EDIT_EOF,       /* Ctrl+D on an empty line */
    EDIT_CANCEL,    /* Ctrl+C: the line was abandoned (and shown as such) */
};

/* What the prompt looks like. `mark` and `after` are invisible sequences
 * (the shell's OSC 133 A and B) written around the prompt each time it is
 * drawn from a fresh origin; `text` may carry SGR, which is not counted as
 * width; `more` starts every line after the first. `home`, when the terminal
 * has one, returns the cursor to where the prompt began (the EmbLink
 * Terminal's OSC 133;R): every redraw starts with it instead of counting rows
 * back up, so a redraw is right even when a resize moved everything while it
 * was on its way. All may be NULL. */
struct edit_prompt { const char *mark, *text, *more, *after, *home; };

struct editor;
struct editor *edit_new(const struct edit_host *host);
void edit_free(struct editor *e);

/* Start a line: draw the prompt. `cols` is the terminal width now (0 =
 * unknown, taken as 80). In EDIT_VT mode this also turns on bracketed paste
 * and in-band resize reports; they are turned off again when the line ends. */
void edit_begin(struct editor *e, const struct edit_prompt *prompt, enum edit_mode mode, int cols);

/* Feed bytes. Stops at the byte that ends the line and says how many it
 * used, so keys typed ahead of the next prompt are not lost. */
enum edit_result edit_feed(struct editor *e, const char *bytes, size_t n, size_t *used);

/* An ESC arrived with nothing after it yet: it may be the Escape key or the
 * start of an arrow's sequence. The caller waits briefly for more bytes and,
 * if none come, calls edit_escape_timeout() -- it was the key. */
bool edit_pending_escape(const struct editor *e);
enum edit_result edit_escape_timeout(struct editor *e);

/* The width changed by some other route than an in-band report (the console,
 * a test). `cleared`: the terminal erased the prompt and put the cursor where
 * it began, so the editor redraws from there. */
void edit_resize(struct editor *e, int cols, bool cleared);

/* The accepted line, valid until the next edit_begin(). */
const char *edit_line(const struct editor *e);

/* The terminal width the editor is drawing for. */
int edit_cols(const struct editor *e);

#endif /* __EMBK_EDIT_H__ */
