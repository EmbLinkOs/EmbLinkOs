/* user/web/css/css.h -- the CSS engine's contract.
 *
 * Split into three files because there are genuinely three concerns here, and
 * they fail in different ways:
 *
 *   decl.c   what a declaration MEANS -- "color: red" -> a field in vstyle.
 *            Pure string->value work. Shared by inline style="" and by rules.
 *   sel.c    what a selector MATCHES, and how strongly. Tree questions and
 *            specificity arithmetic. Knows nothing about properties.
 *   sheet.c  the CASCADE: parse a stylesheet into rules, then for one element
 *            decide which rules win and in what order they apply.
 *
 * The seam docs/BROWSER.md promised holds: all of this writes into the SAME
 * `struct vstyle` the user-agent stylesheet already filled. Nothing downstream
 * -- not the renderer, not layout, not the parser -- changes because CSS
 * arrived. The stylist just has more sources to consult.
 *
 * Bounded like everything else that touches network bytes: a fixed rule table,
 * fixed selector parts, no allocation. An oversized stylesheet loses its tail
 * and says so, rather than growing without limit.
 */
#ifndef _EMBLINK_WEB_CSS_H_
#define _EMBLINK_WEB_CSS_H_

#include <stddef.h>
#include "style.h"

/* ---- decl.c: one declaration block ------------------------------------- */

/* Apply the declarations in `text` ("color:red; font-weight:bold") to `out`.
 * Unknown properties and unparsable values are SKIPPED, never fatal -- CSS's
 * own error handling, and the only sane policy for bytes from a stranger.
 * Returns how many declarations were understood. */
int css_apply_decls(const char *text, size_t len, struct vstyle *out);

/* ---- sel.c: one compound selector --------------------------------------- */

#define CSS_SEL_PARTS 4          /* "nav ul li a" -- deeper is vanishingly rare */

struct css_sel_part {
    char tag[16];                /* "" = any (or '*')          */
    char klass[32];              /* "" = none                  */
    char id[32];                 /* "" = none                  */
};

struct css_sel {
    struct css_sel_part part[CSS_SEL_PARTS];  /* ancestor -> ... -> subject */
    unsigned char n;
    unsigned short spec;         /* specificity: id*100 + class*10 + type */
};

/* Parse ONE selector ("nav ul li.item"). Returns 0, or -1 if unusable. */
int css_sel_parse(const char *s, size_t len, struct css_sel *out);

/* Does `sel` match element `node` in `doc`? Descendant combinator only
 * (whitespace); '>' '+' '~' are parsed as descendant, which is wrong in the
 * safe direction -- a slightly over-eager match, never a missed one. */
struct html_doc;
int css_sel_match(const struct css_sel *sel, struct html_doc *doc, int node);

/* ---- sheet.c: the stylesheet + cascade ---------------------------------- */

#define CSS_MAX_RULES 256

struct css_rule {
    struct css_sel sel;
    const char    *decls;        /* into the caller's stylesheet text */
    size_t         decls_len;
    unsigned short order;        /* document order, for equal specificity */
};

struct css_sheet {
    struct css_rule rules[CSS_MAX_RULES];
    int  n;
    int  truncated;              /* ran out of rule slots */
};

/* Parse a stylesheet. `text` must OUTLIVE the sheet: rules point into it
 * (the same borrow-don't-copy discipline the DOM uses for its arena). */
void css_sheet_parse(struct css_sheet *sheet, const char *text, size_t len);

/* Apply every matching rule to `out`, weakest first, so the strongest wins by
 * landing last. Call AFTER the user-agent stylesheet and BEFORE inline style,
 * which is the cascade's origin order. */
void css_sheet_apply(const struct css_sheet *sheet, struct html_doc *doc,
                     int node, struct vstyle *out);

#endif /* _EMBLINK_WEB_CSS_H_ */
