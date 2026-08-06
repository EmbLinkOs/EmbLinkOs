/* user/web/html.c -- the parser. See html.h for scope. */
#include <string.h>
#include <stdio.h>
#include "html.h"

/* ---- arena ------------------------------------------------------------- */

static int node_new(struct html_doc *d, int kind, int parent) {
    if (d->n >= d->cap) { d->truncated = 1; return -1; }
    struct html_node *n = &d->nodes[d->n];
    memset(n, 0, sizeof *n);
    n->kind = (unsigned char)kind;
    n->first_child = n->next_sibling = -1;
    n->parent = parent;
    int idx = d->n++;
    if (parent >= 0) {                       /* append, keeping source order */
        struct html_node *p = &d->nodes[parent];
        if (p->first_child < 0) p->first_child = idx;
        else {
            int s = p->first_child;
            while (d->nodes[s].next_sibling >= 0) s = d->nodes[s].next_sibling;
            d->nodes[s].next_sibling = idx;
        }
    }
    return idx;
}

static char *str_put(struct html_doc *d, const char *s, size_t len) {
    if (d->strn + len + 1 > d->strcap) { d->truncated = 1; return 0; }
    char *out = d->strs + d->strn;
    memcpy(out, s, len);
    out[len] = 0;
    d->strn += len + 1;
    return out;
}

/* ---- character references ---------------------------------------------- */

/* Only the handful that actually occur in prose, plus numeric. An entity table
 * with two thousand names would be more spec-complete and no more useful; the
 * ones missing render as themselves, which is the failure a reader can see
 * through. */
static int entity(const char *s, size_t len, size_t *used, char *out) {
    static const struct { const char *name; const char *utf8; } tbl[] = {
        { "amp",  "&" }, { "lt",   "<" }, { "gt",   ">" }, { "quot", "\"" },
        { "apos", "'" }, { "nbsp", " " }, { "mdash", "\xE2\x80\x94" },
        { "ndash","\xE2\x80\x93" }, { "hellip", "\xE2\x80\xA6" },
        { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" },
        { "ldquo","\xE2\x80\x9C" }, { "rdquo", "\xE2\x80\x9D" },
        { "lsquo","\xE2\x80\x98" }, { "rsquo", "\xE2\x80\x99" },
    };
    if (len < 2 || s[0] != '&') return 0;
    size_t i = 1;
    if (s[i] == '#') {                                     /* numeric */
        i++;
        int hex = (i < len && (s[i] == 'x' || s[i] == 'X'));
        if (hex) i++;
        unsigned cp = 0; size_t d0 = i;
        while (i < len && s[i] != ';') {
            int c = s[i], v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return 0;
            cp = cp * (hex ? 16 : 10) + (unsigned)v;
            i++;
        }
        if (i >= len || i == d0) return 0;
        i++;                                               /* the ';' */
        /* encode UTF-8 */
        int o = 0;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 63)); }
        else { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 63));
               out[o++] = (char)(0x80 | (cp & 63)); }
        out[o] = 0; *used = i;
        return o;
    }
    size_t ns = i;
    while (i < len && s[i] != ';' && i - ns < 12) i++;
    if (i >= len || s[i] != ';') return 0;
    size_t nl = i - ns;
    for (size_t k = 0; k < sizeof tbl / sizeof tbl[0]; k++) {
        if (strlen(tbl[k].name) == nl && memcmp(tbl[k].name, s + ns, nl) == 0) {
            int o = (int)strlen(tbl[k].utf8);
            memcpy(out, tbl[k].utf8, (size_t)o + 1);
            *used = i + 1;
            return o;
        }
    }
    return 0;
}

/* ---- tag classification ------------------------------------------------ */

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static int is_void(const char *t) {
    static const char *v[] = { "br","img","hr","meta","link","input","area",
                               "base","col","embed","source","track","wbr", 0 };
    for (int i = 0; v[i]; i++) if (ieq(t, v[i])) return 1;
    return 0;
}

/* Elements that close an open one of the same kind. <p>a<p>b is two
 * paragraphs, not a nest -- get this wrong and a real page becomes one
 * infinitely-indented paragraph. */
static int closes_self(const char *t) {
    return ieq(t,"p") || ieq(t,"li") || ieq(t,"dt") || ieq(t,"dd") ||
           ieq(t,"tr") || ieq(t,"td") || ieq(t,"th") || ieq(t,"option");
}

/* Block elements that implicitly close an open <p>. */
static int closes_p(const char *t) {
    static const char *b[] = { "div","p","ul","ol","li","h1","h2","h3","h4","h5",
                               "h6","pre","table","blockquote","hr","section",
                               "article","header","footer","nav","form", 0 };
    for (int i = 0; b[i]; i++) if (ieq(t, b[i])) return 1;
    return 0;
}

/* ---- the parser -------------------------------------------------------- */

struct stack { int idx[64]; int n; };

static void push(struct stack *s, int i) { if (s->n < 64) s->idx[s->n++] = i; }
static int  top(struct stack *s) { return s->n ? s->idx[s->n - 1] : -1; }

/* Trim trailing space from an element's last text child, called when the
 * element closes. Leading/trailing whitespace is only insignificant at a
 * BLOCK's edges -- between inline elements it is a real space, which is why
 * trimming every text run turned "Hello <b>world</b>" into "Helloworld". */
static void trim_tail(struct html_doc *d, int elem) {
    if (elem < 0) return;
    int last = -1;
    for (int c = d->nodes[elem].first_child; c >= 0; c = d->nodes[c].next_sibling) last = c;
    if (last < 0 || d->nodes[last].kind != HTML_TEXT || !d->nodes[last].text) return;
    char *t = d->nodes[last].text;
    size_t l = strlen(t);
    while (l && t[l - 1] == ' ') t[--l] = 0;
}

/* Close the nearest open element named `tag`; if none is open, do nothing --
 * a stray </div> is noise, not a reason to unwind the document. */
static void close_tag(struct html_doc *d, struct stack *s, const char *tag) {
    for (int i = s->n - 1; i >= 0; i--) {
        if (ieq(d->nodes[s->idx[i]].tag, tag)) {
            for (int k = s->n - 1; k >= i; k--) trim_tail(d, s->idx[k]);
            s->n = i;
            return;
        }
    }
}

int html_parse(struct html_doc *d, const char *src, size_t len,
               struct html_node *nodes, int node_cap,
               char *strs, size_t str_cap)
{
    memset(d, 0, sizeof *d);
    d->nodes = nodes; d->cap = node_cap;
    d->strs = strs; d->strcap = str_cap;
    d->root = node_new(d, HTML_ELEM, -1);
    if (d->root < 0) return -1;
    snprintf(d->nodes[d->root].tag, HTML_TAG_MAX, "%s", "document");

    struct stack st = { {0}, 0 };
    push(&st, d->root);

    static char text[16384];        /* the run of text being accumulated */
    size_t tn = 0;

    /* flush accumulated text as a TEXT node, collapsing whitespace runs the
     * way HTML does -- otherwise every newline in the source becomes a gap */
    #define FLUSH() do {                                                     \
        if (tn) {                                                            \
            size_t a = 0, b = tn;                                            \
            /* only the FIRST run in a parent loses its leading space; the    \
             * trailing one is trimmed when the parent closes (trim_tail) */  \
            if (top(&st) < 0 || d->nodes[top(&st)].first_child < 0)           \
                while (a < b && text[a]==' ') a++;                            \
            if (b > a) {                                                     \
                char *p = str_put(d, text + a, b - a);                       \
                if (p) {                                                     \
                    int ti = node_new(d, HTML_TEXT, top(&st));               \
                    if (ti >= 0) d->nodes[ti].text = p;                      \
                }                                                            \
            }                                                                \
            tn = 0;                                                          \
        }                                                                    \
    } while (0)

    size_t i = 0;
    while (i < len) {
        if (src[i] != '<') {
            /* text, with entities decoded and whitespace runs collapsed */
            if (src[i] == '&') {
                char buf[8]; size_t used = 0;
                int o = entity(src + i, len - i, &used, buf);
                if (o) {
                    for (int k = 0; k < o && tn + 1 < sizeof text; k++) text[tn++] = buf[k];
                    i += used;
                    continue;
                }
            }
            char c = src[i++];
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
            if (c == ' ' && tn && text[tn - 1] == ' ') continue;   /* collapse */
            if (tn + 1 < sizeof text) text[tn++] = c;
            continue;
        }

        /* --- a tag --- */
        if (len - i >= 4 && memcmp(src + i, "<!--", 4) == 0) {     /* comment */
            const char *e = 0;
            for (size_t k = i + 4; k + 2 < len; k++)
                if (src[k]=='-' && src[k+1]=='-' && src[k+2]=='>') { e = src + k; break; }
            i = e ? (size_t)(e - src) + 3 : len;
            continue;
        }
        if (len - i >= 2 && src[i+1] == '!') {                     /* doctype */
            while (i < len && src[i] != '>') i++;
            if (i < len) i++;
            continue;
        }

        int closing = (len - i >= 2 && src[i+1] == '/');
        size_t p = i + (closing ? 2 : 1);
        size_t ts = p;
        while (p < len && src[p] != '>' && src[p] != ' ' && src[p] != '\t'
               && src[p] != '\n' && src[p] != '/' ) p++;
        char tag[HTML_TAG_MAX];
        size_t tl = p - ts; if (tl >= HTML_TAG_MAX) tl = HTML_TAG_MAX - 1;
        for (size_t k = 0; k < tl; k++) {
            char c = src[ts + k];
            tag[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        tag[tl] = 0;

        /* attributes: only href/src are kept -- they are the only ones this
         * renderer can act on, and storing the rest would be arena spent on
         * data nobody reads */
        char href[HTML_HREF_MAX]; href[0] = 0;
        while (p < len && src[p] != '>') {
            while (p < len && (src[p]==' '||src[p]=='\t'||src[p]=='\n'||src[p]=='\r')) p++;
            if (p >= len || src[p] == '>' || src[p] == '/') break;
            size_t as = p;
            while (p < len && src[p]!='=' && src[p]!='>' && src[p]!=' ' && src[p]!='\t') p++;
            size_t al = p - as;
            char aname[24]; size_t an = al < sizeof aname - 1 ? al : sizeof aname - 1;
            for (size_t k = 0; k < an; k++) {
                char c = src[as + k];
                aname[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            aname[an] = 0;
            if (p < len && src[p] == '=') {
                p++;
                char q = 0;
                if (p < len && (src[p]=='"' || src[p]=='\'')) { q = src[p]; p++; }
                size_t vs = p;
                if (q) { while (p < len && src[p] != q) p++; }
                else   { while (p < len && src[p]!='>' && src[p]!=' ' && src[p]!='\t') p++; }
                size_t vl = p - vs;
                if ((ieq(aname,"href") || ieq(aname,"src")) && !href[0]) {
                    size_t c = vl < sizeof href - 1 ? vl : sizeof href - 1;
                    memcpy(href, src + vs, c); href[c] = 0;
                }
                if (q && p < len) p++;
            }
        }
        int self_closing = (p > i && src[p-1] == '/');
        if (p < len) p++;                              /* past '>' */

        if (!tag[0]) { i = p; continue; }

        if (closing) {
            FLUSH();
            close_tag(d, &st, tag);
            i = p;
            continue;
        }

        /* <script>/<style>: skip to the matching close without parsing. Their
         * contents are not markup, and treating them as such is how a page
         * ends up displaying its own code. */
        if (ieq(tag, "script") || ieq(tag, "style")) {
            char end[HTML_TAG_MAX + 4];
            snprintf(end, sizeof end, "</%s", tag);
            size_t el = strlen(end);
            size_t k = p;
            while (k + el <= len) {
                int m = 1;
                for (size_t j = 0; j < el; j++) {
                    char a = src[k+j], b = end[j];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (a != b) { m = 0; break; }
                }
                if (m) break;
                k++;
            }
            while (k < len && src[k] != '>') k++;
            i = k < len ? k + 1 : len;
            continue;
        }

        FLUSH();
        if (closes_self(tag) && ieq(d->nodes[top(&st)].tag, tag)) close_tag(d, &st, tag);
        else if (closes_p(tag) && ieq(d->nodes[top(&st)].tag, "p"))  close_tag(d, &st, "p");

        int ni = node_new(d, HTML_ELEM, top(&st));
        if (ni < 0) { i = p; continue; }
        snprintf(d->nodes[ni].tag, HTML_TAG_MAX, "%s", tag);
        if (href[0]) d->nodes[ni].href = str_put(d, href, strlen(href));
        if (!is_void(tag) && !self_closing) push(&st, ni);
        i = p;
    }
    FLUSH();
    for (int k = st.n - 1; k >= 0; k--) trim_tail(d, st.idx[k]);
    #undef FLUSH
    return d->root;
}

/* ---- URL resolution ---------------------------------------------------- */

int html_resolve_url(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !href[0]) return -1;

    /* absolute */
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
        snprintf(out, cap, "%s", href);
        return 0;
    }
    /* find the base's scheme://host boundary */
    const char *p = strstr(base, "://");
    if (!p) return -1;
    const char *hs = p + 3;
    const char *slash = strchr(hs, '/');
    size_t rootlen = slash ? (size_t)(slash - base) : strlen(base);

    if (href[0] == '/') {                       /* root-relative */
        snprintf(out, cap, "%.*s%s", (int)rootlen, base, href);
        return 0;
    }
    /* relative to the base's DIRECTORY -- everything up to the last slash. A
     * base of ".../a/b" makes "c" into ".../a/c", not ".../a/b/c". */
    size_t dirlen = strlen(base);
    while (dirlen > rootlen && base[dirlen - 1] != '/') dirlen--;
    if (dirlen < rootlen) dirlen = rootlen;
    if (dirlen == rootlen) snprintf(out, cap, "%.*s/%s", (int)rootlen, base, href);
    else                   snprintf(out, cap, "%.*s%s", (int)dirlen, base, href);
    return 0;
}
