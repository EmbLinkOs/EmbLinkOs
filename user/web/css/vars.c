/* user/web/css/vars.c -- CSS custom properties.
 *
 * `:root { --bg: #12141a }` and `color: var(--bg)`. Nearly every stylesheet
 * written in the last few years defines its palette and its spacing scale this
 * way, so a browser that cannot resolve var() does not render those pages
 * slightly wrong -- it renders them with no colours at all, because every
 * declaration that uses one is unparseable.
 *
 * DOCUMENT-SCOPED, not element-scoped, and that is a deliberate simplification
 * worth naming. CSS lets any element define a custom property that only its
 * subtree sees, so `.dark { --bg: black }` re-themes one panel. Here every
 * definition goes into one table and the last one parsed wins. That is correct
 * for the overwhelmingly common case -- a single `:root` block at the top of
 * the sheet -- and wrong for per-component theming, which will need the table
 * to hang off the element and inherit. Written down in docs/TODO.md rather
 * than discovered later.
 */
#include <string.h>

#include "css.h"

#define VAR_MAX      64
#define VAR_NAME_MAX 48
#define VAR_VAL_MAX  96

static struct {
    char name[VAR_NAME_MAX];
    char val[VAR_VAL_MAX];
} g_var[VAR_MAX];
static int g_nvar;

void css_vars_reset(void) { g_nvar = 0; }

static void trim(const char **s, size_t *n) {
    while (*n && (**s == ' ' || **s == '\t' || **s == '\n' || **s == '\r')) { (*s)++; (*n)--; }
    while (*n) {
        char c = (*s)[*n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';') (*n)--;
        else break;
    }
}

void css_var_set(const char *name, size_t nn, const char *val, size_t vn) {
    trim(&name, &nn); trim(&val, &vn);
    if (!nn || nn >= VAR_NAME_MAX || vn >= VAR_VAL_MAX) return;
    for (int i = 0; i < g_nvar; i++) {
        if (strlen(g_var[i].name) == nn && !strncmp(g_var[i].name, name, nn)) {
            memcpy(g_var[i].val, val, vn); g_var[i].val[vn] = 0;   /* last wins */
            return;
        }
    }
    if (g_nvar >= VAR_MAX) return;      /* bounded: the tail simply is not set */
    memcpy(g_var[g_nvar].name, name, nn); g_var[g_nvar].name[nn] = 0;
    memcpy(g_var[g_nvar].val, val, vn);  g_var[g_nvar].val[vn] = 0;
    g_nvar++;
}

const char *css_var_get(const char *name, size_t nn) {
    trim(&name, &nn);
    for (int i = 0; i < g_nvar; i++)
        if (strlen(g_var[i].name) == nn && !strncmp(g_var[i].name, name, nn))
            return g_var[i].val;
    return 0;
}

static void css_vars_collect_block(const char *text, size_t len);

/* Scan a declaration block for `--name: value` and register each one. Called
 * at SHEET PARSE time, so a var defined anywhere in the sheet is available to
 * every rule -- including rules written above it, which is what CSS does and
 * what a resolve-as-you-go scheme would get wrong. */
void css_vars_collect(const char *text, size_t len) {
    if (!text) return;
    size_t i = 0;
    while (i < len) {
        /* Find the next declaration BLOCK. Scanning the sheet as though it
         * were one long block does not work: `:root {` begins with a colon, so
         * a property-then-colon reader swallows the selector and the first
         * declaration with it -- which is exactly how the commonest place to
         * define a variable became the one place it was missed. */
        while (i < len && text[i] != '{') i++;
        if (i >= len) break;
        i++;
        size_t end = i;
        int depth = 1;
        while (end < len && depth) {
            if (text[end] == '{') depth++;
            else if (text[end] == '}') depth--;
            if (depth) end++;
        }
        css_vars_collect_block(text + i, end - i);
        i = end < len ? end + 1 : len;
    }
}

/* One declaration block's worth of `--name: value`. */
static void css_vars_collect_block(const char *text, size_t len) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (text[i]==' '||text[i]=='\t'||text[i]=='\n'||
                           text[i]=='\r'||text[i]==';')) i++;
        size_t ps = i;
        while (i < len && text[i] != ':' && text[i] != ';' && text[i] != '}') i++;
        if (i >= len || text[i] != ':') { while (i < len && text[i] != ';') i++; continue; }
        size_t pn = i - ps;
        i++;
        size_t vs = i;
        int depth = 0;
        while (i < len && (depth || (text[i] != ';' && text[i] != '}'))) {
            if (text[i] == '(') depth++;
            else if (text[i] == ')') depth--;
            i++;
        }
        if (pn > 2 && text[ps] == '-' && text[ps + 1] == '-')
            css_var_set(text + ps, pn, text + vs, i - vs);
    }
}

/* Expand every var() in `val` into `out`. Returns 1 if anything was
 * substituted (so the caller can re-read the value), 0 if not.
 *
 * `var(--x, fallback)` uses the fallback when --x is unset, which is how a
 * stylesheet stays readable on a browser that never defined the variable --
 * including this one, before the tail of a long :root block. */
int css_var_expand(const char *val, size_t vn, char *out, size_t cap) {
    size_t o = 0; int did = 0;
    for (size_t i = 0; i < vn && o + 1 < cap; ) {
        if (i + 4 <= vn && !strncmp(val + i, "var(", 4)) {
            size_t j = i + 4, depth = 1, name_s = j, name_n = 0, fb_s = 0, fb_n = 0;
            while (j < vn && depth) {
                if (val[j] == '(') depth++;
                else if (val[j] == ')') { depth--; if (!depth) break; }
                else if (val[j] == ',' && depth == 1 && !name_n) {
                    name_n = j - name_s; fb_s = j + 1;
                }
                j++;
            }
            if (!name_n) name_n = j - name_s; else fb_n = j - fb_s;
            const char *sub = css_var_get(val + name_s, name_n);
            size_t sl;
            const char *src;
            if (sub) { src = sub; sl = strlen(sub); }
            else     { src = val + fb_s; sl = fb_n; }
            while (sl && (*src == ' ' || *src == '\t')) { src++; sl--; }
            if (o + sl >= cap) sl = cap - o - 1;
            memcpy(out + o, src, sl); o += sl;
            did = 1;
            i = (j < vn) ? j + 1 : vn;
            continue;
        }
        out[o++] = val[i++];
    }
    out[o] = 0;
    return did;
}
