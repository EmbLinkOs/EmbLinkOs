/* user/web/css/calc.c -- evaluating calc().
 *
 * `width: calc(100% - 240px)` is how a sidebar-and-content layout is written
 * without flexbox, and how nearly every "fill the rest" rule was written before
 * it. The expression cannot be reduced to a number when the stylesheet is read,
 * because the percentage is against a containing block that does not exist yet.
 *
 * So it is not reduced to a number. It is reduced to a LINEAR EXPRESSION --
 * `pct` percent of the containing block plus `px` pixels -- which is a form the
 * layout engine can finish once it knows the container. That covers every calc
 * a page actually writes; what it does not cover is a calc whose percentage is
 * multiplied by something (`calc(100% * 0.4)` is fine, `calc(100% * 100%)` is
 * not a length at all) and nested percentages inside min()/max().
 *
 * Precedence is the real one: * and / bind tighter than + and -, and
 * parentheses nest. Getting that wrong turns `calc(100% - 2 * 20px)` into
 * something that is off by exactly one gap, which is the kind of wrong that
 * looks like a rounding bug.
 */
#include <string.h>

#include "css.h"

struct term { float pct; float px; };   /* pct% of the container, plus px */

struct calc_p {
    const char *s;
    size_t n, i;
    int bad;
};

static void skip_ws(struct calc_p *p) {
    while (p->i < p->n && (p->s[p->i]==' '||p->s[p->i]=='\t'||
                           p->s[p->i]=='\n'||p->s[p->i]=='\r')) p->i++;
}

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int unit_is(struct calc_p *p, const char *u) {
    size_t k = 0;
    while (u[k] && p->i + k < p->n && ci(p->s[p->i + k]) == u[k]) k++;
    if (u[k]) return 0;
    /* the unit must END here -- otherwise "em" matches the front of "empty" */
    size_t after = p->i + k;
    if (after < p->n) {
        char c = ci(p->s[after]);
        if (c >= 'a' && c <= 'z') return 0;
    }
    p->i += k;
    return 1;
}

static struct term parse_sum(struct calc_p *p);

/* a number with a unit, or a parenthesised sub-expression */
static struct term parse_atom(struct calc_p *p) {
    struct term t = { 0, 0 };
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '(') {
        p->i++;
        t = parse_sum(p);
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ')') p->i++;
        else p->bad = 1;
        return t;
    }
    int neg = 0;
    if (p->i < p->n && (p->s[p->i] == '-' || p->s[p->i] == '+')) {
        neg = (p->s[p->i] == '-'); p->i++;
    }
    float v = 0; int seen = 0;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        v = v * 10.0f + (float)(p->s[p->i] - '0'); p->i++; seen = 1;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++; float f = 0.1f;
        while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
            v += (float)(p->s[p->i] - '0') * f; f *= 0.1f; p->i++; seen = 1;
        }
    }
    if (!seen) { p->bad = 1; return t; }
    if (neg) v = -v;

    if      (unit_is(p, "px"))  t.px = v;
    else if (unit_is(p, "rem")) t.px = v * 16.0f;
    else if (unit_is(p, "em"))  t.px = v * 16.0f;
    else if (unit_is(p, "vw"))  t.px = v * css_viewport_w() / 100.0f;
    else if (unit_is(p, "vh"))  t.px = v * css_viewport_h() / 100.0f;
    else if (p->i < p->n && p->s[p->i] == '%') { p->i++; t.pct = v; }
    else t.px = v;              /* a bare number: a multiplier, or px */
    return t;
}

/* * and / -- one side must be a plain number, because a length times a length
 * is an area and not something this can place on a page. */
static struct term parse_product(struct calc_p *p) {
    struct term a = parse_atom(p);
    for (;;) {
        skip_ws(p);
        if (p->i >= p->n) break;
        char op = p->s[p->i];
        if (op != '*' && op != '/') break;
        p->i++;
        struct term b = parse_atom(p);
        if (op == '*') {
            if (b.pct == 0) { a.px *= b.px; a.pct *= b.px; }
            else if (a.pct == 0 && a.px != 0) { float k = a.px; a.px = b.px * k; a.pct = b.pct * k; }
            else p->bad = 1;
        } else {
            if (b.pct != 0 || b.px == 0) p->bad = 1;
            else { a.px /= b.px; a.pct /= b.px; }
        }
    }
    return a;
}

static struct term parse_sum(struct calc_p *p) {
    struct term a = parse_product(p);
    for (;;) {
        skip_ws(p);
        if (p->i >= p->n) break;
        char op = p->s[p->i];
        if (op != '+' && op != '-') break;
        p->i++;
        struct term b = parse_product(p);
        if (op == '+') { a.px += b.px; a.pct += b.pct; }
        else           { a.px -= b.px; a.pct -= b.pct; }
    }
    return a;
}

int css_calc(const char *s, size_t n, float *out_pct, float *out_px) {
    if (!s || !n) return -1;
    /* skip a leading `calc` */
    size_t i = 0;
    while (i < n && (s[i]==' '||s[i]=='\t')) i++;
    if (i + 4 <= n && ci(s[i])=='c' && ci(s[i+1])=='a' && ci(s[i+2])=='l' && ci(s[i+3])=='c')
        i += 4;
    else return -1;
    while (i < n && (s[i]==' '||s[i]=='\t')) i++;
    if (i >= n || s[i] != '(') return -1;
    i++;
    /* the matching close paren */
    size_t depth = 1, j = i;
    while (j < n && depth) {
        if (s[j] == '(') depth++;
        else if (s[j] == ')') { depth--; if (!depth) break; }
        j++;
    }
    struct calc_p p = { s, j, i, 0 };
    struct term t = parse_sum(&p);
    skip_ws(&p);
    if (p.bad || p.i != p.n) return -1;
    *out_pct = t.pct;
    *out_px  = t.px;
    return 0;
}
