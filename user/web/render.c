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
#include "url.h"
#include "css.h"
#include "imgcache.h"
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

/* An <img>. The base URL is needed to resolve src, the cache is what turns a
 * URL into pixels, and the content width is what an oversized picture gets
 * clamped to -- none of which belong in the DOM, so all three are held for the
 * render pass like the stylesheet is. */
static const char *g_base;
static float g_content_w;

/* Emit one picture, or a stand-in that occupies THE SAME SPACE.
 *
 * Reserving the box before the bytes arrive is the whole point. A picture that
 * appears at its natural size after the page has been laid out shoves the
 * paragraph the reader is in the middle of -- the single most irritating thing
 * a browser does, and it is entirely avoidable whenever the markup or the
 * stylesheet said how big the picture is.
 *
 * Size is decided in cascade order (CSS beats the markup's attributes beats the
 * picture's own natural size), then clamped to the content width with the
 * ASPECT PRESERVED, because a wide image that overflows its column is worse
 * than a smaller one. */
static void emit_image(struct html_doc *d, int n, const struct vstyle *st) {
    const char *src = d->nodes[n].href;      /* the parser stores src here */
    if (!src || !src[0]) return;

    char url[512];
    if (url_resolve(g_base ? g_base : "", src, url, sizeof url) != 0)
        snprintf(url, sizeof url, "%s", src);

    struct img_slot *s = imgcache_want(url);
    if (s && (s->state == IMG_WANTED || s->state == IMG_LOADING)) em_request_frame();

    int ready = s && s->state == IMG_READY && s->px;

    /* --- how big? CSS, then the attributes, then what arrived --- */
    float w = 0, h = 0;
    if (st->width)  w = (float)st->width;
    if (st->height) h = (float)st->height;
    if (!w && d->nodes[n].img_w) w = (float)d->nodes[n].img_w;
    if (!h && d->nodes[n].img_h) h = (float)d->nodes[n].img_h;

    /* One dimension stated, the other implied by the picture's own shape --
     * which is only knowable once it has arrived. */
    if (ready) {
        float nw = (float)s->w, nh = (float)s->h;
        if (w && !h) h = nh * (w / nw);
        else if (h && !w) w = nw * (h / nh);
        else if (!w && !h) { w = nw; h = nh; }
    }
    if (w <= 0 || h <= 0) {
        /* Nothing to reserve honestly: no stated size and no picture yet.
         * Alt text is the right stand-in -- inventing a box height would move
         * the page exactly as much as not reserving one. */
        const char *alt = d->nodes[n].alt;
        if (alt && alt[0]) { Text(alt).font(font_for(st)).color(ui_theme()->text_secondary); return; }
        Text((s && s->state == IMG_FAILED) ? "\xE2\x9C\x95" : "\xE2\x97\xAF").caption().tertiary();
        return;
    }

    /* --- clamp to the column, preserving the aspect --- */
    if (g_content_w > 16.0f && w > g_content_w) {
        h *= g_content_w / w;
        w  = g_content_w;
    }

    if (ready) {
        em_flush();
        ui_image_sized((uint64_t)(uintptr_t)s->px, s->px, s->w, s->h, w, h);
        return;
    }

    /* The reserved box: the picture's space, held open, faintly outlined so it
     * reads as "something is coming" rather than as a rendering hole. */
    em_flush();
    ui_begin_vstack(0x1A6E0000ULL ^ (uint64_t)(uintptr_t)s);
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = w },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = h });
    ui_set_corner_radius(4);
    { struct paint pl = { 0 };
      pl.kind = PAINT_SOLID;
      pl.solid = ui_theme()->surface_alt;
      pl.solid.a *= (s && s->state == IMG_FAILED) ? 0.35f : 0.6f;
      ui_set_paint(pl); }
    ui_end_stack();
}

/* Is this subtree inline-only? An inline run ends where a block begins. */
static int is_inline(struct html_doc *d, int n, const struct vstyle *parent) {
    if (d->nodes[n].kind == HTML_TEXT) return 1;
    struct vstyle s;
    vstyle_for_node(d, n, parent, g_sheet, &s);
    return s.display == VD_INLINE || s.display == VD_IMAGE;
}

static void render_block(struct html_doc *d, int node, const struct vstyle *s,
                         const char *href, int list_index);

/* One inline child, at any depth. RECURSIVE on purpose: this was a
 * hand-unrolled three-level walk that only knew about text, so an <img> (or
 * any element) nested inside an inline wrapper was silently dropped -- a
 * <figure><img></figure> rendered as nothing at all, with no error anywhere.
 * Depth is bounded so hostile markup cannot recurse us to death. */
static void emit_inline(struct html_doc *d, int c, const struct vstyle *st,
                        const char *href, int depth) {
    if (depth > 8) return;
    if (d->nodes[c].kind == HTML_TEXT) {
        if (d->nodes[c].text) emit_text(d->nodes[c].text, st, href);
        return;
    }
    struct vstyle s;
    vstyle_for_node(d, c, st, g_sheet, &s);
    if (s.display == VD_NONE) return;
    /* AFTER computing its own style, not before: an image is styled by the
     * rules that match IT (`.half { width: 150px }`), and handing it the
     * parent's vstyle silently ignored every one of them. */
    if (s.display == VD_IMAGE) { emit_image(d, c, &s); return; }
    const char *h = d->nodes[c].href ? d->nodes[c].href : href;
    for (int k = d->nodes[c].first_child; k >= 0; k = d->nodes[k].next_sibling)
        emit_inline(d, k, &s, h, depth + 1);
}

/* Walk a run of inline siblings [from, to) into one wrapping row. */
static void render_inline_run(struct html_doc *d, int from, int to,
                              const struct vstyle *parent, const char *href) {
    /* spacing 0: the inter-word space is baked into each word instead (see
     * emit_text). A uniform gap between boxes puts one in front of a comma
     * that arrived as its own text node -- "bold , italic". */
    Flow(.spacing = 0) {
        for (int c = from; c >= 0 && c != to; c = d->nodes[c].next_sibling)
            emit_inline(d, c, parent, href, 0);
    }
}

/* --- tables ---------------------------------------------------------------
 *
 * A table is the one document structure whose columns must line up ACROSS
 * independent rows -- which is precisely what a row of HStacks cannot do and
 * what the layout engine's grid already does. So a <table> becomes one grid of
 * N columns and every cell is a child of it, in reading order.
 *
 * The honest limit, stated because it is visible: the grid's tracks are EQUAL
 * width. A real table sizes each column to its content, which needs a
 * measurement pass the renderer cannot do (it emits; layout measures later).
 * Equal columns keep every row aligned -- the property that makes a table a
 * table -- and cost some space in a table of one short column and one long
 * one. Content-proportional tracks are a layout-engine feature, logged.
 */

/* Cells of one row, in order; descends through thead/tbody/tfoot, which carry
 * no box of their own. Returns the count. */
static int row_cells(struct html_doc *d, int row, int *out, int max,
                     const struct vstyle *pst) {
    int n = 0;
    for (int c = d->nodes[row].first_child; c >= 0 && n < max; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_ELEM) continue;
        struct vstyle cs;
        vstyle_for_node(d, c, pst, g_sheet, &cs);
        if (cs.display == VD_CELL) out[n++] = c;
    }
    return n;
}

/* Every row of a table, descending through row groups. */
static int table_rows(struct html_doc *d, int tbl, int *out, int max,
                      const struct vstyle *tst) {
    int n = 0;
    for (int c = d->nodes[tbl].first_child; c >= 0 && n < max; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_ELEM) continue;
        struct vstyle cs;
        vstyle_for_node(d, c, tst, g_sheet, &cs);
        if (cs.display == VD_ROW) { out[n++] = c; continue; }
        if (cs.display == VD_BLOCK) {            /* thead/tbody/tfoot: descend */
            for (int r = d->nodes[c].first_child; r >= 0 && n < max; r = d->nodes[r].next_sibling) {
                if (d->nodes[r].kind != HTML_ELEM) continue;
                struct vstyle rs;
                vstyle_for_node(d, r, &cs, g_sheet, &rs);
                if (rs.display == VD_ROW) out[n++] = r;
            }
        }
    }
    return n;
}

#define TBL_MAX_ROWS 128
#define TBL_MAX_COLS  12

static void render_children(struct html_doc *d, int node, const struct vstyle *s,
                            const char *href);

static void render_table(struct html_doc *d, int node, const struct vstyle *st,
                         const char *href) {
    static int rows[TBL_MAX_ROWS];
    int nrow = table_rows(d, node, rows, TBL_MAX_ROWS, st);
    if (!nrow) return;

    /* The column count is the WIDEST row: a row with fewer cells leaves the
     * tail empty rather than shifting the ones after it into the wrong
     * column, which is what makes a ragged table still readable. */
    int ncol = 1;
    for (int i = 0; i < nrow; i++) {
        static int cells[TBL_MAX_COLS];
        int nc = row_cells(d, rows[i], cells, TBL_MAX_COLS, st);
        int span_total = 0;
        for (int c = 0; c < nc; c++) {
            int sp = d->nodes[cells[c]].img_w;   /* colspan, parsed into img_w */
            span_total += sp > 0 ? sp : 1;
        }
        if (span_total > ncol) ncol = span_total;
    }
    if (ncol > TBL_MAX_COLS) ncol = TBL_MAX_COLS;

    const struct ui_theme *t = ui_theme();
    VStack(.spacing = 0, .align = Fill,
           .pt = (float)st->margin_top, .pb = (float)st->margin_bottom) {
        /* the caption, if the author wrote one: a table's title belongs above
         * it and outside the grid */
        for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling) {
            if (d->nodes[c].kind != HTML_ELEM) continue;
            struct vstyle cs;
            vstyle_for_node(d, c, st, g_sheet, &cs);
            if (cs.display != VD_CAPTION) continue;
            VStack(.spacing = 0, .align = Fill, .pb = (float)cs.margin_bottom) {
                render_children(d, c, &cs, href);
            }
        }

        em_flush();
        ui_begin_vstack(0x7AB10000ULL ^ (uint64_t)(uintptr_t)&d->nodes[node]);
        ui_set_grid(ncol, 0.0f, 0.0f);
        ui_set_size((struct layout_size){ .mode = SIZE_FLEX, .flex_grow = 1 },
                    (struct layout_size){ .mode = SIZE_INTRINSIC });

        for (int i = 0; i < nrow; i++) {
            static int cells[TBL_MAX_COLS];
            int nc = row_cells(d, rows[i], cells, TBL_MAX_COLS, st);
            int placed = 0;
            for (int c = 0; c < nc && placed < ncol; c++) {
                int cell = cells[c];
                struct vstyle rs, cs;
                vstyle_for_node(d, rows[i], st, g_sheet, &rs);
                vstyle_for_node(d, cell, &rs, g_sheet, &cs);
                int sp = d->nodes[cell].img_w;
                if (sp < 1) sp = 1;
                if (placed + sp > ncol) sp = ncol - placed;

                em_flush();
                ui_begin_vstack(0x7AB20000ULL ^ (uint64_t)(uintptr_t)&d->nodes[cell]);
                ui_set_grid_span(sp);
                ui_set_padding(7, 9, 7, 9);
                ui_set_align(ALIGN_STRETCH);
                /* A header row needs to READ as one, and a row separator is
                 * what stops a dense table becoming a wall. Both come from the
                 * theme so the table follows the desktop into dark mode. */
                if (cs.bold) {
                    struct paint hp = { 0 };
                    hp.kind = PAINT_SOLID; hp.solid = t->surface_alt;
                    ui_set_paint(hp);
                } else {
                    struct color line = t->text_secondary; line.a *= 0.16f;
                    ui_set_border(1.0f, line);
                }
                render_children(d, cell, &cs, href);
                em_flush();
                ui_end_stack();
                placed += sp;
            }
            /* pad a short row so the next one starts in column 0 */
            for (; placed < ncol; placed++) {
                em_flush();
                ui_begin_vstack(0x7AB30000ULL ^ ((uint64_t)i << 8) ^ (uint64_t)placed);
                ui_set_padding(7, 9, 7, 9);
                ui_end_stack();
            }
        }
        em_flush();
        ui_end_stack();
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
    if (s->display == VD_IMAGE) { emit_image(d, node, s); return; }
    if (s->display == VD_TABLE) { render_table(d, node, s, href); return; }
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
    return vellum_render_page(d, root, sheet, 0);
}

const char *vellum_render_page(struct html_doc *d, int root,
                               const struct css_sheet *sheet, const char *base) {
    return vellum_render_sized(d, root, sheet, base, 0.0f);
}

const char *vellum_render_sized(struct html_doc *d, int root,
                                const struct css_sheet *sheet, const char *base,
                                float content_w) {
    g_base = base;
    g_content_w = content_w;
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
