/* user/web/render.c -- document tree + computed styles -> EmUI nodes.
 *
 * This is where the CSS box model meets a layout engine that was built for
 * application UI, and the mapping is the whole trick:
 *
 *   a BLOCK      becomes a vertical stack. Blocks stack down the page; that is
 *                what a column does. Margins become padding.
 *   an INLINE    run becomes a Flow -- EmUI's real flex-wrap row -- holding one
 *                text node PER WORD. Wrapping between words is then the layout
 *                engine's job, not ours, and it already does it well.
 *   a LIST ITEM  is a row of [marker][block], so the text hangs correctly under
 *                itself rather than wrapping back under the bullet.
 *   <pre>        is a block that neither collapses whitespace nor wraps: one
 *                text node per source line.
 *
 * Splitting inline text into per-word nodes is the single decision that makes
 * this work. It costs nodes, and it buys correct wrapping, correct mixed
 * styling within a paragraph, and per-word hit testing for links -- which is
 * how a link that wraps across two lines stays clickable on both.
 *
 * This file reads `struct vstyle` and NEVER a tag name. See docs/BROWSER.md §4.
 */
#include <string.h>
#include <stdio.h>

#include "ui.h"
#include "em.h"
#include "theme.h"
#include "html.h"
#include "style.h"
#include "css.h"
#include "render.h"

static void (*g_on_link)(const char *href);
static const char *g_hover_href;      /* link under the pointer, for the status line */

/* Link targets must outlive the frame that emits them: EmUI keeps the pointer.
 * The document arena owns the href strings, so this only needs to survive
 * until the click is acted on -- the app copies before navigating. */
static const char *g_pending;
/* The document's author stylesheet for this render pass. Held for the pass
 * rather than threaded through every function, because EVERY style question
 * needs it and passing it down eight call sites would be noise. */
static const struct css_sheet *g_sheet;

const char *vellum_hovered_link(void) { return g_hover_href; }

static EmFont font_for(const struct vstyle *s) {
    /* Size and WEIGHT are independent in CSS, and the toolkit's roles must not
     * conflate them: `font-size: 19px` with no `font-weight` is a large
     * paragraph, not a heading. Subtitle is the large regular face. */
    if (s->size == 3) return s->bold ? Heading : Subtitle;
    if (s->size == 2) return s->bold ? Title   : Subtitle;
    if (s->size == 1) return Caption;
    return s->bold ? BodyBold : Body;
}

static Color color_for(const struct vstyle *s) {
    const struct ui_theme *t = ui_theme();
    /* An AUTHOR colour wins -- that is what the cascade decided. Absent one,
     * the THEME decides, so an unstyled page follows the desktop into dark
     * mode instead of being black-on-white in the middle of it. */
    if (s->color) {
        Color c;
        c.r = (float)((s->color >> 16) & 0xFF) / 255.0f;
        c.g = (float)((s->color >>  8) & 0xFF) / 255.0f;
        c.b = (float)( s->color        & 0xFF) / 255.0f;
        c.a = (float)((s->color >> 24) & 0xFF) / 255.0f;
        if (c.a <= 0.0f) c.a = 1.0f;
        return c;
    }
    if (s->link) return t->accent;
    if (s->mono) return t->text_secondary;   /* code reads as a quieter voice */
    return t->text;
}

/* One word of an inline run. A link's words are BUTTONS so each is clickable
 * on its own -- which is what keeps a link that wraps across a line break
 * clickable on both halves. */
static void emit_word(const char *w, const struct vstyle *s, const char *href) {
    if (href && g_on_link) {
        if (Button(w).ghost().font(font_for(s)).py(0).px(2)
                .color(color_for(s)).id(w).clicked()) {
            g_pending = href;
        }
        return;
    }
    Text(w).font(font_for(s)).color(color_for(s));
}

/* Emit a text run word by word into the surrounding Flow. */
static void emit_text(const char *txt, const struct vstyle *s, const char *href) {
    static char word[256];
    size_t n = 0;
    /* Did the SOURCE end this run with a space? "and " before an <i> did, and
     * dropping it welds the words either side of the tag together
     * ("italicand"). The space belongs to the text, not to the loop. */
    size_t tl = strlen(txt);
    int trail = tl && txt[tl - 1] == ' ';
    /* ...and a LEADING one for the same reason from the other side. " and "
     * after a </i> carries its space in front; the word loop drops leading
     * whitespace, so the space that separated the tag from the next word
     * disappeared and you read "italicand". The first word carries it. */
    int lead = tl && txt[0] == ' ';
    int first = 1;
    for (const char *p = txt; ; p++) {
        if (*p && *p != ' ') {
            if (n + 1 < sizeof word) word[n++] = *p;
            continue;
        }
        if (n) {
            word[n] = 0;
            /* the pool: EmUI keeps the pointer for the frame, so a stack
             * buffer reused per word would render the LAST word everywhere */
            static char pool[512][68];
            static int  pn;
            if (pn >= 512) pn = 0;
            /* trailing space unless the run ends here: the space is part of
             * the word box, so a following comma sits flush against it */
            snprintf(pool[pn], sizeof pool[0], "%s%s%s",
                     (first && lead) ? " " : "", word, (*p || trail) ? " " : "");
            first = 0;
            emit_word(pool[pn], s, href);
            pn++;
            n = 0;
        }
        if (!*p) break;
    }
}

/* Is this subtree inline-only? An inline run ends where a block begins. */
static int is_inline(struct html_doc *d, int n, const struct vstyle *parent) {
    if (d->nodes[n].kind == HTML_TEXT) return 1;
    struct vstyle s;
    vstyle_for_node(d, n, parent, g_sheet, &s);
    return s.display == VD_INLINE;
}

static void render_block(struct html_doc *d, int node, const struct vstyle *s,
                         const char *href, int list_index);

/* Walk a run of inline siblings [from, to) into one wrapping row. */
static void render_inline_run(struct html_doc *d, int from, int to,
                              const struct vstyle *parent, const char *href) {
    /* spacing 0: the inter-word space is baked into each word instead (see
     * emit_text). A uniform gap between boxes puts one in front of a comma
     * that arrived as its own text node -- "bold , italic". */
    Flow(.spacing = 0) {
        for (int c = from; c >= 0 && c != to; c = d->nodes[c].next_sibling) {
            if (d->nodes[c].kind == HTML_TEXT) {
                if (d->nodes[c].text) emit_text(d->nodes[c].text, parent, href);
            } else {
                struct vstyle s;
                vstyle_for_node(d, c, parent, g_sheet, &s);
                if (s.display == VD_NONE) continue;
                const char *h = d->nodes[c].href ? d->nodes[c].href : href;
                /* an inline element's children join the SAME run, so a <b>
                 * mid-sentence does not start a new line */
                for (int k = d->nodes[c].first_child; k >= 0; k = d->nodes[k].next_sibling) {
                    if (d->nodes[k].kind == HTML_TEXT) {
                        if (d->nodes[k].text) emit_text(d->nodes[k].text, &s, h);
                    } else {
                        struct vstyle s2;
                        vstyle_for_node(d, k, &s, g_sheet, &s2);
                        const char *h2 = d->nodes[k].href ? d->nodes[k].href : h;
                        for (int m = d->nodes[k].first_child; m >= 0; m = d->nodes[m].next_sibling)
                            if (d->nodes[m].kind == HTML_TEXT && d->nodes[m].text)
                                emit_text(d->nodes[m].text, &s2, h2);
                    }
                }
            }
        }
    }
}

/* <pre>: one text node per source line, whitespace intact, no wrapping. */
static void render_pre(struct html_doc *d, int node, const struct vstyle *s) {
    for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_TEXT || !d->nodes[c].text) continue;
        static char pool[128][200];
        static int pn;
        const char *p = d->nodes[c].text;
        while (*p) {
            size_t n = 0;
            char line[200];
            while (*p && *p != '\n' && n + 1 < sizeof line) line[n++] = *p++;
            line[n] = 0;
            if (*p == '\n') p++;
            if (pn >= 128) pn = 0;
            snprintf(pool[pn], sizeof pool[0], "%s", line[0] ? line : " ");
            Text(pool[pn]).font(Caption).color(ui_theme()->text_secondary);
            pn++;
        }
    }
}

static void render_children(struct html_doc *d, int node, const struct vstyle *s,
                            const char *href) {
    int c = d->nodes[node].first_child;
    int li = 0;
    while (c >= 0) {
        if (is_inline(d, c, s)) {
            int start = c;
            while (c >= 0 && is_inline(d, c, s)) c = d->nodes[c].next_sibling;
            render_inline_run(d, start, c, s, href);   /* c is the run's end */
        } else {
            struct vstyle cs;
            vstyle_for_node(d, c, s, g_sheet, &cs);
            if (cs.display != VD_NONE) {
                if (cs.display == VD_LIST_ITEM) li++;
                render_block(d, c, &cs, d->nodes[c].href ? d->nodes[c].href : href, li);
            }
            c = d->nodes[c].next_sibling;
        }
    }
}

static void render_block(struct html_doc *d, int node, const struct vstyle *s,
                         const char *href, int list_index) {
    if (s->display == VD_LIST_ITEM) {
        /* [marker][content] as a ROW, so wrapped text hangs under itself
         * instead of sliding back under the bullet */
        HStack(.spacing = 8, .align = Leading, .grow = 1,
               .pb = (float)s->margin_bottom) {
            Text("\xE2\x80\xA2").caption().tertiary();
            VStack(.spacing = 2, .align = Fill, .grow = 1) {
                render_children(d, node, s, href);
            }
        }
        (void)list_index;
        return;
    }
    /* .align = Fill, NOT Leading. A leading-aligned block sizes to its
     * content, so the Flow inside it is handed an unbounded width and never
     * wraps -- the first render ran every paragraph off the right edge. A
     * block in a document is as wide as its parent; that is what makes the
     * line breaks happen. */
    VStack(.spacing = 2, .align = Fill,
           .pt = (float)s->margin_top, .pb = (float)s->margin_bottom,
           .pl = (float)s->indent) {
        if (s->pre) render_pre(d, node, s);
        else        render_children(d, node, s, href);
    }
}

const char *vellum_render(struct html_doc *d, int root) {
    return vellum_render_styled(d, root, 0);
}

const char *vellum_render_styled(struct html_doc *d, int root,
                                 const struct css_sheet *sheet) {
    g_pending = 0;
    g_hover_href = 0;
    g_sheet = sheet;
    struct vstyle rs;
    vstyle_root(&rs);
    if (root >= 0) render_block(d, root, &rs, 0, 0);
    g_sheet = 0;
    return g_pending;
}

void vellum_set_link_handler(void (*fn)(const char *href)) { g_on_link = fn; }
