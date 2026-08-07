/* user/web/css/sel.c -- what a selector MATCHES, and how strongly.
 *
 * Tree questions and specificity arithmetic. This file knows nothing about
 * properties: give it a selector and an element and it answers yes or no.
 * That separation is why the cascade in sheet.c stays short.
 *
 * Supported: type (`p`), class (`.item`), id (`#main`), universal (`*`),
 * compounds (`li.item`, `a#home.x`) and the descendant combinator (space).
 *
 * Combinators are REAL: `>` (child), `+` (adjacent sibling) and `~` (general
 * sibling) each mean what they say. They used to be parsed as descendant,
 * which over-matched -- `nav > ul` also styled a `ul` three levels down inside
 * a nav -- and an author who scoped a rule tightly got it applied widely. That
 * was the right first approximation (dropping the rule loses the intent
 * entirely) and it stops being right once pages are the target rather than
 * documents we wrote ourselves.
 *
 * Also supported: `:first-child` and `:last-child`.
 *
 * Still NOT supported, and still skipped rather than dropped: attribute
 * selectors and every other pseudo-class. `a:hover` styling `a` is closer to
 * the author's page than no rule at all.
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
        /* the combinator between the previous compound and this one; the last
         * one written wins, so "a  >  b" is a child combinator and not two */
        int comb = CSS_COMB_DESC;
        while (i < len && (is_ws(s[i]) || s[i]=='>' || s[i]=='+' || s[i]=='~')) {
            if      (s[i] == '>') comb = CSS_COMB_CHILD;
            else if (s[i] == '+') comb = CSS_COMB_ADJ;
            else if (s[i] == '~') comb = CSS_COMB_SIB;
            i++;
        }
        if (i >= len) break;

        struct css_sel_part *pt = &out->part[out->n];
        pt->comb = (unsigned char)comb;
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
                char lead = s[i];
                i++;
                if (i < len && s[i] == ':') i++;      /* ::before -- an element */
                size_t vs = i;
                while (i < len && !is_ws(s[i]) && s[i]!='.' && s[i]!='#' &&
                       s[i]!='>' && s[i]!='+' && s[i]!='~') i++;
                size_t vn = i - vs;
                if (lead == ':') {
                    char name[24];
                    cpy_lower(name, sizeof name, s + vs, vn);
                    if (!strcmp(name, "first-child")) { pt->first_child = 1; out->spec += 10; got = 1; }
                    else if (!strcmp(name, "last-child")) { pt->last_child = 1; out->spec += 10; got = 1; }
                    /* anything else: skip the token and keep the compound --
                     * `a:hover` still styling `a` beats dropping the rule */
                }
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

/* The previous ELEMENT sibling, or -1. The DOM stores forward links only, so
 * this is a walk from the parent -- which is fine: sibling combinators are
 * rare and the lists are short. */
static int prev_elem_sibling(struct html_doc *d, int n) {
    int p = d->nodes[n].parent;
    if (p < 0) return -1;
    int prev = -1;
    for (int c = d->nodes[p].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        if (c == n) return prev;
        if (d->nodes[c].kind == HTML_ELEM) prev = c;
    }
    return -1;
}

static int is_first_elem_child(struct html_doc *d, int n) {
    return prev_elem_sibling(d, n) < 0;
}

static int is_last_elem_child(struct html_doc *d, int n) {
    for (int c = d->nodes[n].next_sibling; c >= 0; c = d->nodes[c].next_sibling)
        if (d->nodes[c].kind == HTML_ELEM) return 0;
    return 1;
}

static int part_matches(const struct css_sel_part *pt, struct html_doc *d, int n) {
    const struct html_node *e = &d->nodes[n];
    if (e->kind != HTML_ELEM) return 0;
    if (pt->first_child && !is_first_elem_child(d, n)) return 0;
    if (pt->last_child  && !is_last_elem_child(d, n))  return 0;
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

    /* Then the remaining parts, right to left, each reached by ITS OWN
     * combinator -- the one recorded on the part that follows it.
     *
     * Descendant and general-sibling are greedy: they take the first candidate
     * that matches and never back up. That is not strictly correct for
     * pathological selectors ("a a b" against nested a's) and it is right for
     * every selector a real document uses, while being unable to loop. */
    int cur = node;
    while (k > 0) {
        int comb = sel->part[k].comb;
        const struct css_sel_part *want = &sel->part[k - 1];
        if (comb == CSS_COMB_CHILD) {
            cur = d->nodes[cur].parent;
            if (cur < 0 || !part_matches(want, d, cur)) return 0;
        } else if (comb == CSS_COMB_ADJ) {
            cur = prev_elem_sibling(d, cur);
            if (cur < 0 || !part_matches(want, d, cur)) return 0;
        } else if (comb == CSS_COMB_SIB) {
            int p = prev_elem_sibling(d, cur);
            while (p >= 0 && !part_matches(want, d, p)) p = prev_elem_sibling(d, p);
            if (p < 0) return 0;
            cur = p;
        } else {
            int a = d->nodes[cur].parent;
            while (a >= 0 && !part_matches(want, d, a)) a = d->nodes[a].parent;
            if (a < 0) return 0;
            cur = a;
        }
        k--;
    }
    return 1;
}
