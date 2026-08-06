/* user/web/style.h -- computed styles: the seam CSS will arrive through.
 *
 * Everything downstream of this file reads ONLY `struct vstyle`. The renderer
 * never asks what tag a node is, never matches a selector, never looks at an
 * attribute. That is the whole point: when CSS lands (docs/BROWSER.md §4) it
 * changes how this struct gets FILLED and nothing else -- not the parser, not
 * the renderer, not layout.
 *
 * v1 fills it from a user-agent stylesheet: a table of sensible defaults per
 * tag, which is what every browser did before CSS existed and what every
 * browser still falls back to. About a hundred lines, and it is the difference
 * between a document you can read and a wall of undifferentiated text.
 */
#ifndef _EMBLINK_WEB_STYLE_H_
#define _EMBLINK_WEB_STYLE_H_

enum { VD_INLINE = 0, VD_BLOCK, VD_LIST_ITEM, VD_NONE };
enum { VM_NONE = 0, VM_BULLET, VM_DECIMAL };

struct vstyle {
    unsigned char display;
    unsigned char size;        /* 0 body, 1 caption, 2 title, 3 heading */
    unsigned char bold, italic, mono, underline, link, pre;
    unsigned char marker;      /* VM_* -- how a list item is bulleted   */
    short margin_top, margin_bottom, indent;
};

/* The document root's style: everything inherits from this. */
void vstyle_root(struct vstyle *out);

/* Compute `tag`'s style given its parent's. Inheritable properties (size,
 * weight, italic, mono, colour-ish flags) descend; box properties (display,
 * margins, indent, marker) do not -- that split is CSS's inheritance model and
 * getting it wrong makes a <b> inside a heading reset to body text. */
void vstyle_for(const char *tag, const struct vstyle *parent, struct vstyle *out);

#endif /* _EMBLINK_WEB_STYLE_H_ */
