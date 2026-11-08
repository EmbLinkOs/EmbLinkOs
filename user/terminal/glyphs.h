/* glyphs.h -- the characters Terminal draws itself instead of taking from a
 * font: box drawing (U+2500-257F), block elements (U+2580-259F), braille
 * (U+2800-28FF) and the Powerline separators (U+E0B0-E0BF).
 *
 * A font draws them to its own idea of the line height, so a frame of box
 * lines shows seams between rows and a Powerline segment a sliver of
 * background at its point. Drawn here, they fill the cell exactly as it is
 * -- whatever its size, at any font size -- and meet the next cell edge to
 * edge. Pure: a coverage mask out, host-tested (tests/tty/render_test.c). */
#ifndef EMBLINK_TERM_GLYPHS_H
#define EMBLINK_TERM_GLYPHS_H
#include <stdbool.h>
#include <stdint.h>

bool term_glyph_is_procedural(uint32_t cp);
/* Draw `cp` into `mask`: w*h coverage bytes, row by row, cleared first. `line`
 * is a light line's thickness in pixels (heavy is twice it). Returns false,
 * leaving the mask clear, for a codepoint this does not draw. */
bool term_glyph_draw(uint32_t cp, uint8_t *mask, int w, int h, int line);
#endif
