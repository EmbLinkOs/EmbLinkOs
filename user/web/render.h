/* user/web/render.h -- document + computed styles -> EmUI. See render.c.
 *
 * Reads `struct vstyle` and never a tag name, so CSS changes nothing here. */
#ifndef _EMBLINK_WEB_RENDER_H_
#define _EMBLINK_WEB_RENDER_H_

#include "html.h"

/* Emit the document as EmUI nodes. Call inside a frame, in a scroll view.
 * Returns the href of a link clicked THIS frame, or NULL -- the caller copies
 * it before navigating, because the string lives in the document arena that
 * navigation is about to reuse. */
const char *vellum_render(struct html_doc *doc, int root);

/* ...and the same with the document's own stylesheet applied. `sheet` may be
 * NULL, which is exactly vellum_render(). */
struct css_sheet;
const char *vellum_render_styled(struct html_doc *doc, int root,
                                 const struct css_sheet *sheet);

/* Non-NULL enables link rendering (words become clickable). */
void vellum_set_link_handler(void (*fn)(const char *href));

/* The link under the pointer, for a status line. NULL when none. */
const char *vellum_hovered_link(void);

#endif
