/* ==========================================================================
 * wcwidth.h -- how many terminal cells a codepoint takes.
 *
 * ONE table, shared by the shell's line editor and the Terminal's screen model
 * (user/terminal/vt.c calls it too). They must agree to the cell: the editor
 * moves the cursor by counting cells, the terminal places glyphs by counting
 * them, and a single disagreement -- an emoji one thinks is narrow -- leaves
 * the cursor one column off for the rest of the line.
 * ========================================================================== */
#ifndef __EMBK_WCWIDTH_H__
#define __EMBK_WCWIDTH_H__

#include <stdint.h>

/* -1 for a control character, 0 for a combining mark (it joins the cell
 * before), 2 for East Asian wide/fullwidth and emoji, 1 otherwise. */
int wcwidth_cp(uint32_t cp);

/* Decode one UTF-8 sequence at s (n bytes available). Returns the bytes used
 * (1 for a malformed or truncated byte, which decodes as U+FFFD) and the
 * codepoint in *cp. */
int utf8_decode(const char *s, int n, uint32_t *cp);

#endif /* __EMBK_WCWIDTH_H__ */
