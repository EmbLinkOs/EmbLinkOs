/* ==========================================================================
 * highlight.h -- the colours of the line being typed.
 *
 * The line editor (shell/edit) asks how each byte is drawn; this answers from
 * the lexer's tokens and what the SHELL knows, as zsh-syntax-highlighting's
 * main highlighter answers for zsh (its default styles, edit.h): a command
 * the shell would run -- a builtin, a `def`, a program in /system/bin or
 * /data/apps -- green, a word in its place that names none red and bold,
 * keywords and braces yellow, quoted text yellow with its escapes cyan, `$(`
 * and its `)` magenta, redirections yellow, comments bold black, and a word
 * that names a file underlined (one on its way to a file's name too, while it
 * ends the line).
 * ========================================================================== */
#ifndef __EMBK_HIGHLIGHT_H__
#define __EMBK_HIGHLIGHT_H__

#include <stdbool.h>
#include <stddef.h>

/* An edit_host.highlight: `ctx` is the session's top scope. */
void shell_highlight(void *ctx, const char *text, size_t n, bool done, unsigned char *style);

/* The programs in /system/bin and /data/apps are listed once per prompt:
 * call this before each, so the next line looks again. */
void shell_highlight_fresh(void);

#endif /* __EMBK_HIGHLIGHT_H__ */
