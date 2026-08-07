/* user/web/select.h -- selecting text on a page, and taking it away.
 *
 * A browser you cannot copy out of is a browser you can only look at. This is
 * the machinery for dragging across a document, showing what is selected, and
 * putting it on the system clipboard.
 *
 * WORD GRANULARITY, deliberately. The renderer already emits one scene node per
 * word -- that is what keeps a link clickable on both sides of a line break --
 * so the word is the grain the document is already cut along. Selecting from
 * the middle of one word to the middle of another would mean measuring glyph
 * prefixes through the font engine on every pointer move, and would still have
 * to reconstruct the run's text from the DOM to copy a partial word. Snapping
 * to whole words costs the user very little and costs the implementation an
 * order of magnitude less; it is a real limitation and it is written down in
 * docs/TODO.md rather than hidden.
 *
 * The model is an INDEX RANGE over the words in document order. Two facts make
 * that sound: render.c emits words in document order, and the scene tree keeps
 * children in the order they were emitted. So the Nth word the renderer emits
 * is the Nth text node a depth-first walk finds -- which is what lets geometry
 * (from the walk, after layout) and identity (from the emitter, during build)
 * refer to the same word without either side storing a pointer to the other.
 */
#ifndef _EMBLINK_WEB_SELECT_H_
#define _EMBLINK_WEB_SELECT_H_

#include <stddef.h>

/* Reset for a new document. Any live selection is dropped -- it indexed words
 * that no longer exist. */
void vsel_reset(void);

/* --- the app's side (called from vellum.c, on input) --------------------- */

/* Pointer went down / moved / came up at window coordinates (x,y). `down`
 * begins a drag (and clears any previous selection), `move` extends it, `up`
 * ends it. Returns 1 if the selection changed and the page must be redrawn. */
int  vsel_pointer(float x, float y, int down, int held);

/* Select the whole document. Returns 1 if anything changed. */
int  vsel_all(void);
/* Drop the selection. Returns 1 if there was one. */
int  vsel_clear(void);
/* Is anything selected? */
int  vsel_active(void);

/* Write the selected text into `out`. Returns the number of bytes written, or
 * 0 if nothing was selected. Words are joined by spaces and rows by newlines,
 * so a copied paragraph pastes as a paragraph.
 *
 * It does NOT touch the clipboard: producing the text and deciding where it
 * goes are separate jobs, and keeping the syscall out of here is what lets the
 * whole selection be exercised by `make browser-render` in two seconds instead
 * of in a five-minute boot. */
size_t vsel_copy_text(char *out, size_t cap);

/* Rebuild the word geometry from the laid-out scene. Called once per frame
 * AFTER layout, because a word's position is not known until then. */
void vsel_sync_geometry(void);

#endif /* _EMBLINK_WEB_SELECT_H_ */
