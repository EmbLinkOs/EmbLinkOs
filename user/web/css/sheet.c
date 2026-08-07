/* user/web/css/sheet.c -- the CASCADE.
 *
 * Parse a stylesheet into rules, then for one element decide which rules
 * apply and in what ORDER. Order is the whole point: the cascade is not "find
 * the winning rule", it is "apply every matching rule weakest-first and let
 * the strongest land last", because different rules contribute different
 * properties and they all have to survive.
 *
 * Sorting is by (specificity, document order) -- CSS's own tie-break, and the
 * reason two rules with the same selector strength resolve by which came
 * later rather than by luck.
 *
 * The rule table is FIXED. A stylesheet that overflows it loses its tail and
 * sets `truncated`, the same bargain the DOM arena makes: bounded appetite for
 * bytes that came from a stranger, and honesty about what was dropped.
 */
#include <string.h>

#include "html.h"
#include "css.h"

/* Skip /* ... *\/ comments and whitespace. */
static size_t skip_junk(const char *s, size_t len, size_t i) {
    for (;;) {
        while (i < len && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'||s[i]=='\f')) i++;
        if (i + 1 < len && s[i]=='/' && s[i+1]=='*') {
            i += 2;
            while (i + 1 < len && !(s[i]=='*' && s[i+1]=='/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        return i;
    }
}

void css_sheet_parse(struct css_sheet *sheet, const char *text, size_t len) {
    if (!sheet) return;
    memset(sheet, 0, sizeof *sheet);
    if (!text || !len) return;

    size_t i = 0;
    unsigned short order = 0;
    while (i < len) {
        i = skip_junk(text, len, i);
        if (i >= len) break;

        /* An at-rule (@media, @import, @font-face). We cannot evaluate the
         * condition, so we SKIP the whole thing rather than apply rules that
         * were meant for print or for a phone. Dropping is the safe direction
         * here -- unlike a combinator, a wrongly-applied @media block can
         * rewrite the entire page. */
        if (text[i] == '@') {
            int depth = 0;
            while (i < len) {
                if (text[i] == '{') depth++;
                else if (text[i] == '}') { depth--; if (depth <= 0) { i++; break; } }
                else if (text[i] == ';' && depth == 0) { i++; break; }
                i++;
            }
            continue;
        }

        /* selector list, up to '{' */
        size_t ss = i;
        while (i < len && text[i] != '{') i++;
        if (i >= len) break;
        size_t se = i;
        i++;                                        /* past '{' */

        /* declaration block, to the matching '}' */
        size_t ds = i;
        while (i < len && text[i] != '}') i++;
        size_t de = i;
        if (i < len) i++;                           /* past '}' */

        /* one rule per comma-separated selector: they share declarations but
         * each carries its OWN specificity, which is why they cannot be one
         * rule with a list inside */
        size_t p = ss;
        while (p < se) {
            size_t cs = p;
            while (p < se && text[p] != ',') p++;
            size_t ce = p;
            if (p < se) p++;
            while (cs < ce && (text[cs]==' '||text[cs]=='\t'||text[cs]=='\n'||text[cs]=='\r')) cs++;
            while (ce > cs && (text[ce-1]==' '||text[ce-1]=='\t'||text[ce-1]=='\n'||text[ce-1]=='\r')) ce--;
            if (ce <= cs) continue;

            if (sheet->n >= CSS_MAX_RULES) { sheet->truncated = 1; return; }
            struct css_rule *r = &sheet->rules[sheet->n];
            if (css_sel_parse(text + cs, ce - cs, &r->sel) != 0) continue;
            r->decls = text + ds;
            r->decls_len = de - ds;
            r->order = order++;
            sheet->n++;
        }
    }
}

void css_sheet_apply(const struct css_sheet *sheet, struct html_doc *doc,
                     int node, struct vstyle *out) {
    if (!sheet || !doc || !out || node < 0 || node >= doc->n) return;

    /* Collect the matches, then apply in cascade order. Two passes rather than
     * one because "weakest first" is not the order they are found in. */
    unsigned char idx[CSS_MAX_RULES];
    int m = 0;
    for (int i = 0; i < sheet->n && m < CSS_MAX_RULES; i++)
        if (css_sel_match(&sheet->rules[i].sel, doc, node)) idx[m++] = (unsigned char)i;
    if (!m) return;

    /* insertion sort by (specificity, document order) -- ascending, so the
     * strongest rule is applied LAST and therefore wins each property it
     * sets, while weaker rules still contribute the properties it doesn't */
    for (int a = 1; a < m; a++) {
        unsigned char key = idx[a];
        const struct css_rule *rk = &sheet->rules[key];
        int b = a - 1;
        while (b >= 0) {
            const struct css_rule *rb = &sheet->rules[idx[b]];
            int heavier = rb->sel.spec > rk->sel.spec ||
                          (rb->sel.spec == rk->sel.spec && rb->order > rk->order);
            if (!heavier) break;
            idx[b + 1] = idx[b];
            b--;
        }
        idx[b + 1] = key;
    }

    for (int a = 0; a < m; a++)
        css_apply_decls(sheet->rules[idx[a]].decls, sheet->rules[idx[a]].decls_len, out);
}
