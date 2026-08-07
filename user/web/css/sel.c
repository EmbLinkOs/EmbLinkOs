/* user/web/css/sel.c -- what a selector MATCHES, and how strongly.
 *
 * Tree questions and specificity arithmetic. This file knows nothing about
 * properties: give it a selector and an element and it answers yes or no.
 * That separation is why the cascade in sheet.c stays short.
 *
 * Supported: type (`p`), class (`.item`), id (`#main`), universal (`*`),
 * compounds (`li.item`, `a#home.x`) and the descendant combinator (space).
 *
 * NOT supported, and parsed as descendant on purpose: `>` `+` `~`, attribute
 * selectors, pseudo-classes. Treating a child combinator as a descendant
 * over-matches -- it styles some elements the author scoped more tightly --
 * which degrades a page's appearance. Treating it as no-match would DROP the
 * rule entirely, which loses the author's intent completely. Erring toward
 * "applies too widely" keeps the page looking closer to what was meant.
 */
#include <string.h>

#include "html.h"
#include "css.h"

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

static void cpy_lower(char *dst, size_t cap, const char *s, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) dst[k++] = (char)ci(s[i]);
    dst[k] = 0;
}

int css_sel_parse(const char *s, size_t len, struct css_sel *out) {
    if (!s || !out) return -1;
    memset(out, 0, sizeof *out);

    size_t i = 0;
    while (i < len && out->n < CSS_SEL_PARTS) {
        while (i < len && (is_ws(s[i]) || s[i]=='>' || s[i]=='+' || s[i]=='~')) i++;
        if (i >= len) break;

        struct css_sel_part *pt = &out->part[out->n];
        int got = 0;
        while (i < len && !is_ws(s[i]) && s[i]!='>' && s[i]!='+' && s[i]!='~') {
            if (s[i] == '.' || s[i] == '#') {
                char kind = s[i]; i++;
                size_t vs = i;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~' && s[i]!=':' && s[i]!='[') i++;
                if (i > vs) {
                    if (kind == '.') { cpy_lower(pt->klass, sizeof pt->klass, s + vs, i - vs);
                                       out->spec += 10; }
                    else             { cpy_lower(pt->id, sizeof pt->id, s + vs, i - vs);
                                       out->spec += 100; }
                    got = 1;
                }
            } else if (s[i] == ':' || s[i] == '[') {
                /* a pseudo-class or attribute test we cannot evaluate. Skip
                 * the token and keep the rest of the compound: `a:hover`
                 * still styling `a` is closer to the author's page than
                 * dropping the rule. */
                i++;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~') i++;
            } else if (s[i] == '*') {
                i++; got = 1;                       /* any element, spec 0 */
            } else {
                size_t vs = i;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~' && s[i]!=':' && s[i]!='[') i++;
                if (i > vs) {
                    cpy_lower(pt->tag, sizeof pt->tag, s + vs, i - vs);
                    out->spec += 1;
                    got = 1;
                }
            }
        }
        if (got) out->n++;
    }
    return out->n ? 0 : -1;
}

/* class="a b c" -- match one name against the whitespace-separated list */
static int has_class(const char *list, const char *want) {
    if (!list || !want || !*want) return 0;
    size_t wl = strlen(want);
    for (const char *p = list; *p; ) {
        while (*p && is_ws(*p)) p++;
        const char *s = p;
        while (*p && !is_ws(*p)) p++;
        size_t n = (size_t)(p - s);
        if (n == wl) {
            size_t k = 0;
            while (k < n && ci(s[k]) == want[k]) k++;
            if (k == n) return 1;
        }
    }
    return 0;
}

static int part_matches(const struct css_sel_part *pt, struct html_doc *d, int n) {
    const struct html_node *e = &d->nodes[n];
    if (e->kind != HTML_ELEM) return 0;
    if (pt->tag[0]) {
        size_t i = 0;
        while (pt->tag[i] && e->tag[i] && ci(e->tag[i]) == pt->tag[i]) i++;
        if (pt->tag[i] || e->tag[i]) return 0;
    }
    if (pt->klass[0] && !has_class(e->klass, pt->klass)) return 0;
    if (pt->id[0]) {
        if (!e->id) return 0;
        size_t i = 0;
        while (pt->id[i] && e->id[i] && ci(e->id[i]) == pt->id[i]) i++;
        if (pt->id[i] || e->id[i]) return 0;
    }
    return 1;
}

int css_sel_match(const struct css_sel *sel, struct html_doc *d, int node) {
    if (!sel || !d || sel->n == 0 || node < 0 || node >= d->n) return 0;

    /* The SUBJECT (last part) must match the element itself. */
    int k = sel->n - 1;
    if (!part_matches(&sel->part[k], d, node)) return 0;

    /* Then walk ancestors, matching the remaining parts right to left. This
     * is greedy and therefore not strictly correct for pathological selectors
     * ("a a b" against nested a's), but it is right for every selector a real
     * document uses and it cannot loop. */
    int cur = d->nodes[node].parent;
    k--;
    while (k >= 0 && cur >= 0) {
        if (part_matches(&sel->part[k], d, cur)) k--;
        cur = d->nodes[cur].parent;
    }
    return k < 0;
}
