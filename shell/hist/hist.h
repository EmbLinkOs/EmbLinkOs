/* ==========================================================================
 * hist.h -- the shell's command history.
 *
 * Lives in the SHELL, not the terminal -- the same split Linux has (readline
 * is bash's, not xterm's): the shell owns the line, so it is the one that
 * recalls and searches what was typed (shell/edit).
 *
 * A ring of the last HIST_MAX lines, each its own allocation (a pasted
 * multi-line program is one entry), oldest falling off. With a file opened
 * (hist_open), every new line is appended to it as it is accepted, so the
 * history outlives the session and every Terminal tab adds to the same one;
 * a session reads the file once, when it starts, as bash does.
 *
 * The file is one entry per line, with backslash and newline escaped
 * ("\\" and "\n"), so an entry spanning lines stays one entry.
 * ========================================================================== */
#ifndef __EMBK_HIST_H__
#define __EMBK_HIST_H__

#include <stddef.h>

#define HIST_MAX 10000      /* entries kept; oldest falls off */

/* Record a submitted line. Empty lines are never stored, and a line
 * identical to the most recent one is collapsed (bash's `ignoredups`) --
 * hammering Enter shouldn't flood the ring. With a file open, the line is
 * appended to it too. */
void hist_push(const char *line);

/* How many entries are live (saturates at HIST_MAX). */
size_t hist_count(void);

/* Entry `i`, 0 = OLDEST .. count-1 = newest. NULL if out of range.
 * Borrowed: valid until that entry falls off the ring. */
const char *hist_get(size_t i);

/* Load the history file at `path` (a missing file is an empty history) and
 * append every later hist_push to it. A file grown past twice HIST_MAX lines
 * is rewritten with the newest HIST_MAX. Returns the entries loaded; a file
 * that cannot be opened loads nothing, and appends that fail are dropped --
 * history then simply stays in memory. -1 only for an over-long path. */
int hist_open(const char *path);

/* Forget everything in memory and stop appending (tests). */
void hist_reset(void);

#endif /* __EMBK_HIST_H__ */
