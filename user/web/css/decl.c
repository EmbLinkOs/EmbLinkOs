/* user/web/css/decl.c -- what a declaration MEANS.
 *
 * "color: #c00; font-weight: bold" -> fields in a struct vstyle. That is the
 * whole job: no selectors, no cascade, no tree. It is shared verbatim by
 * inline style="" and by rules in a stylesheet, which is why it is its own
 * file -- the two callers arrive from opposite directions and must agree
 * perfectly about what a value means.
 *
 * Every property here is one this renderer can actually honour. A parser that
 * accepts `float: left` and then ignores it has not implemented floats; it has
 * implemented a lie that is harder to find than a missing feature.
 */
#include <string.h>
#include <stdlib.h>

#include "html.h"
#include "css.h"

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int tok_eq(const char *s, size_t n, const char *w) {
    size_t wl = strlen(w);
    if (n != wl) return 0;
    for (size_t i = 0; i < n; i++) if (ci(s[i]) != w[i]) return 0;
    return 1;
}
static int tok_has(const char *s, size_t n, const char *w) {   /* substring */
    size_t wl = strlen(w);
    if (wl > n) return 0;
    for (size_t i = 0; i + wl <= n; i++) {
        size_t j = 0;
        while (j < wl && ci(s[i+j]) == w[j]) j++;
        if (j == wl) return 1;
    }
    return 0;
}

/* A length in px. "12px", "12", "1.5em" (em ~ 16px, close enough for a
 * document). Returns 0 and sets *ok=0 for things we cannot honour, so the
 * caller can leave the property alone rather than write a wrong number. */
static short len_px(const char *s, size_t n, int *ok) {
    *ok = 0;
    size_t i = 0;
    int neg = 0;
    while (i < n && (s[i]==' '||s[i]=='\t')) i++;
    if (i < n && (s[i]=='-'||s[i]=='+')) { neg = (s[i]=='-'); i++; }
    if (i >= n || s[i] < '0' || s[i] > '9') {
        if (tok_has(s, n, "auto")) { *ok = 1; return 0; }
        return 0;
    }
    double v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (i < n && s[i] == '.') {
        i++; double f = 0.1;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v += (s[i]-'0') * f; f *= 0.1; i++; }
    }
    if (tok_has(s + i, n - i, "em") || tok_has(s + i, n - i, "rem")) v *= 16.0;
    else if (tok_has(s + i, n - i, "%")) { return 0; }   /* percentages need a container */
    *ok = 1;
    if (v > 400) v = 400;                                 /* a margin, not a canvas */
    return (short)(neg ? -v : v);
}

/* A colour. #rgb, #rrggbb, and the handful of names a document actually uses.
 * Returns 0 if unrecognised -- vstyle treats 0 as "no author colour". */
static unsigned css_color(const char *s, size_t n) {
    while (n && (*s==' '||*s=='\t')) { s++; n--; }
    while (n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]==';')) n--;
    if (n && *s == '#') {
        unsigned v = 0; size_t d = 0;
        for (size_t i = 1; i < n; i++) {
            int c = ci(s[i]), h;
            if (c >= '0' && c <= '9') h = c - '0';
            else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
            else break;
            v = (v << 4) | (unsigned)h; d++;
        }
        if (d == 3) {   /* #rgb -> #rrggbb */
            unsigned r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
            v = (r * 17u << 16) | (g * 17u << 8) | (b * 17u);
            d = 6;
        }
        if (d == 6) return 0xFF000000u | v;
        return 0;
    }
    static const struct { const char *name; unsigned rgb; } named[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},
        {"blue",0x0000FF},{"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},
        {"maroon",0x800000},{"navy",0x000080},{"teal",0x008080},{"olive",0x808000},
        {"purple",0x800080},{"orange",0xFFA500},{"yellow",0xFFFF00},{"lime",0x00FF00},
        {"aqua",0x00FFFF},{"cyan",0x00FFFF},{"fuchsia",0xFF00FF},{"magenta",0xFF00FF},
    };
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (tok_eq(s, n, named[i].name)) return 0xFF000000u | named[i].rgb;
    return 0;
}

int css_apply_decls(const char *text, size_t len, struct vstyle *out) {
    if (!text || !out) return 0;
    int applied = 0;
    size_t i = 0;
    while (i < len) {
        /* property */
        while (i < len && (text[i]==' '||text[i]=='\t'||text[i]=='\n'||
                           text[i]=='\r'||text[i]==';')) i++;
        if (i >= len) break;
        size_t ps = i;
        while (i < len && text[i] != ':' && text[i] != ';' && text[i] != '}') i++;
        size_t pn = i - ps;
        while (pn && (text[ps+pn-1]==' '||text[ps+pn-1]=='\t')) pn--;
        if (i >= len || text[i] != ':') {            /* junk: skip to the ';' */
            while (i < len && text[i] != ';') i++;
            continue;
        }
        i++;                                          /* past ':' */
        /* value */
        while (i < len && (text[i]==' '||text[i]=='\t')) i++;
        size_t vs = i;
        while (i < len && text[i] != ';' && text[i] != '}') i++;
        size_t vn = i - vs;
        while (vn && (text[vs+vn-1]==' '||text[vs+vn-1]=='\t'||
                      text[vs+vn-1]=='\n'||text[vs+vn-1]=='\r')) vn--;
        const char *p = text + ps, *v = text + vs;
        if (!pn || !vn) continue;

        int ok = 0;
        if (tok_eq(p, pn, "color")) {
            unsigned c = css_color(v, vn);
            if (c) { out->color = c; ok = 1; }
        } else if (tok_eq(p, pn, "font-weight")) {
            if (tok_eq(v, vn, "bold") || tok_eq(v, vn, "bolder")) { out->bold = 1; ok = 1; }
            else if (tok_eq(v, vn, "normal") || tok_eq(v, vn, "lighter")) { out->bold = 0; ok = 1; }
            else { int n2 = atoi(v); if (n2 >= 100 && n2 <= 900) { out->bold = n2 >= 600; ok = 1; } }
        } else if (tok_eq(p, pn, "font-style")) {
            if (tok_eq(v, vn, "italic") || tok_eq(v, vn, "oblique")) { out->italic = 1; ok = 1; }
            else if (tok_eq(v, vn, "normal")) { out->italic = 0; ok = 1; }
        } else if (tok_eq(p, pn, "font-family")) {
            /* the only family distinction this renderer HAS is mono vs not */
            if (tok_has(v, vn, "mono") || tok_has(v, vn, "courier") ||
                tok_has(v, vn, "consol")) { out->mono = 1; ok = 1; }
            else { out->mono = 0; ok = 1; }
        } else if (tok_eq(p, pn, "font-size")) {
            /* mapped onto the four roles the toolkit has, by px threshold --
             * an honest approximation, not a pretend continuum */
            int lok = 0; short px = len_px(v, vn, &lok);
            if (tok_eq(v, vn, "small") || tok_eq(v, vn, "x-small")) { out->size = 1; ok = 1; }
            else if (tok_eq(v, vn, "large") || tok_eq(v, vn, "x-large")) { out->size = 2; ok = 1; }
            else if (tok_eq(v, vn, "xx-large")) { out->size = 3; ok = 1; }
            else if (lok && px > 0) {
                out->size = px >= 24 ? 3 : px >= 19 ? 2 : px <= 13 ? 1 : 0;
                ok = 1;
            }
        } else if (tok_eq(p, pn, "text-decoration") || tok_eq(p, pn, "text-decoration-line")) {
            if (tok_has(v, vn, "underline")) { out->underline = 1; ok = 1; }
            else if (tok_has(v, vn, "none")) { out->underline = 0; ok = 1; }
        } else if (tok_eq(p, pn, "display")) {
            if (tok_eq(v, vn, "none"))        { out->display = VD_NONE;      ok = 1; }
            else if (tok_eq(v, vn, "block"))  { out->display = VD_BLOCK;     ok = 1; }
            else if (tok_eq(v, vn, "inline")) { out->display = VD_INLINE;    ok = 1; }
            else if (tok_eq(v, vn, "list-item")) { out->display = VD_LIST_ITEM; ok = 1; }
            else if (tok_eq(v, vn, "inline-block")) { out->display = VD_INLINE; ok = 1; }
        } else if (tok_eq(p, pn, "margin-top")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->margin_top = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin-bottom")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->margin_bottom = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin-left") || tok_eq(p, pn, "padding-left")) {
            int lok = 0; short px = len_px(v, vn, &lok);
            if (lok) { out->indent = px; ok = 1; }
        } else if (tok_eq(p, pn, "margin") || tok_eq(p, pn, "padding")) {
            /* the shorthand, in its four spellings. Only the vertical parts
             * and the left indent are things this renderer can express. */
            size_t k = 0; short vals[4]; int nv = 0;
            while (k < vn && nv < 4) {
                while (k < vn && (v[k]==' '||v[k]=='\t')) k++;
                size_t s2 = k;
                while (k < vn && v[k]!=' ' && v[k]!='\t') k++;
                if (k > s2) { int lok = 0; short px = len_px(v + s2, k - s2, &lok); vals[nv++] = lok ? px : 0; }
            }
            if (nv == 1) { out->margin_top = out->margin_bottom = vals[0]; out->indent = vals[0]; ok = 1; }
            else if (nv == 2) { out->margin_top = out->margin_bottom = vals[0]; out->indent = vals[1]; ok = 1; }
            else if (nv >= 3) { out->margin_top = vals[0]; out->margin_bottom = vals[2];
                                out->indent = nv >= 4 ? vals[3] : vals[1]; ok = 1; }
        }
        if (ok) applied++;
    }
    return applied;
}
