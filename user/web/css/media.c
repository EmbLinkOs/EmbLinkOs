/* user/web/css/media.c -- evaluating a media query.
 *
 * `@media (min-width: 700px) { ... }`. Until this existed the whole block was
 * skipped, which was the safe direction while nothing could evaluate it -- a
 * wrongly-applied media block rewrites the entire page -- but it means a
 * mobile-first stylesheet loses every desktop rule it has. Most sites are
 * written that way now: the base rules are the phone layout and everything
 * else lives behind a min-width. Skipping them does not make such a page
 * slightly narrower, it renders the phone version at desktop size.
 *
 * The environment is set by the app (the window's content width, and whether
 * the desktop is dark) rather than assumed here, because the browser is not the
 * only thing that will want to ask.
 *
 * Supported: media types (`all`, `screen`, `print`, `speech`, with an optional
 * `only`), `and`, comma-separated alternatives, a leading `not`, and the
 * features that decide layout in practice -- min/max width and height,
 * orientation, and prefers-color-scheme. An UNRECOGNISED feature makes its
 * conjunction false, which is what CSS says and is also the safe direction:
 * a query we do not understand does not get to restyle the page.
 */
#include <string.h>

#include "css.h"

static float g_vw = 940.0f, g_vh = 620.0f;
static int   g_dark = 1;          /* this desktop is dark by default */

void css_media_set(float w, float h, int dark) {
    if (w > 0) g_vw = w;
    if (h > 0) g_vh = h;
    g_dark = dark ? 1 : 0;
}

static int ci(char c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

/* token compare, case-insensitive */
static int teq(const char *s, size_t n, const char *w) {
    size_t i = 0;
    for (; i < n && w[i]; i++) if (ci(s[i]) != w[i]) return 0;
    return i == n && !w[i];
}

/* A length in px. em/rem are against the 15px body this renderer uses, which
 * is the same assumption decl.c makes -- one number, one place to change. */
static float len_px(const char *s, size_t n) {
    while (n && is_ws(*s)) { s++; n--; }
    float v = 0; size_t i = 0; int seen = 0, frac = 0; float scale = 1;
    for (; i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'); i++) {
        if (s[i] == '.') { frac = 1; continue; }
        if (frac) { scale *= 0.1f; v += (float)(s[i] - '0') * scale; }
        else v = v * 10.0f + (float)(s[i] - '0');
        seen = 1;
    }
    if (!seen) return -1.0f;
    if (i + 1 < n && (ci(s[i]) == 'e') && ci(s[i+1]) == 'm') return v * 15.0f;
    if (i + 2 < n && (ci(s[i]) == 'r') && ci(s[i+1]) == 'e' && ci(s[i+2]) == 'm') return v * 15.0f;
    return v;
}

/* One `(feature: value)` test, or a bare `(feature)`. */
static int feature_matches(const char *s, size_t n) {
    while (n && is_ws(*s)) { s++; n--; }
    while (n && is_ws(s[n-1])) n--;
    size_t c = 0;
    while (c < n && s[c] != ':') c++;
    const char *name = s; size_t nn = c;
    while (nn && is_ws(name[nn-1])) nn--;
    const char *val = (c < n) ? s + c + 1 : 0;
    size_t vn = (c < n) ? n - c - 1 : 0;

    if (teq(name, nn, "min-width"))  return val && len_px(val, vn) <= g_vw;
    if (teq(name, nn, "max-width"))  return val && len_px(val, vn) >= g_vw;
    if (teq(name, nn, "width"))      return val && len_px(val, vn) == g_vw;
    if (teq(name, nn, "min-height")) return val && len_px(val, vn) <= g_vh;
    if (teq(name, nn, "max-height")) return val && len_px(val, vn) >= g_vh;
    if (teq(name, nn, "orientation")) {
        if (!val) return 0;
        while (vn && is_ws(*val)) { val++; vn--; }
        while (vn && is_ws(val[vn-1])) vn--;
        return teq(val, vn, g_vw >= g_vh ? "landscape" : "portrait");
    }
    if (teq(name, nn, "prefers-color-scheme")) {
        if (!val) return 0;
        while (vn && is_ws(*val)) { val++; vn--; }
        while (vn && is_ws(val[vn-1])) vn--;
        return teq(val, vn, g_dark ? "dark" : "light");
    }
    /* A feature we do not know cannot be claimed to hold. */
    return 0;
}

/* One conjunction: `screen and (min-width: 700px) and (orientation: landscape)`,
 * optionally led by `not`. */
static int conjunction_matches(const char *s, size_t n) {
    int negate = 0, result = 1, any = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && is_ws(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '(') {
            size_t depth = 1, j = i + 1;
            while (j < n && depth) {
                if (s[j] == '(') depth++;
                else if (s[j] == ')') depth--;
                if (depth) j++;
            }
            if (!feature_matches(s + i + 1, j - i - 1)) result = 0;
            any = 1;
            i = j < n ? j + 1 : n;
            continue;
        }
        size_t ts = i;
        while (i < n && !is_ws(s[i]) && s[i] != '(') i++;
        size_t tn = i - ts;
        if (!tn) continue;
        if      (teq(s + ts, tn, "not"))  negate = 1;
        else if (teq(s + ts, tn, "and"))  ;               /* joins, no-op */
        else if (teq(s + ts, tn, "only")) ;               /* legacy guard  */
        else if (teq(s + ts, tn, "all") || teq(s + ts, tn, "screen")) { any = 1; }
        else if (teq(s + ts, tn, "print") || teq(s + ts, tn, "speech")) {
            /* We are a screen. A print-only block must not restyle the page. */
            result = 0; any = 1;
        } else {
            /* an unknown media type */
            result = 0; any = 1;
        }
    }
    if (!any) return 0;                     /* an empty query matches nothing */
    return negate ? !result : result;
}

int css_media_matches(const char *q, size_t n) {
    if (!q) return 0;
    while (n && is_ws(*q)) { q++; n--; }
    while (n && is_ws(q[n-1])) n--;
    /* `@media { ... }` with no query at all is `all` -- rare, but a page that
     * writes it means "always". */
    if (!n) return 1;
    /* comma-separated alternatives: any one of them is enough */
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || q[i] == ',') {
            if (conjunction_matches(q + start, i - start)) return 1;
            start = i + 1;
        }
    }
    return 0;
}
