/* screentext.c -- see screentext.h. */
#include "screentext.h"
#include <stdlib.h>
#include <string.h>

/* --- logical lines ---------------------------------------------------------------- */

static bool push(struct term_text *t, uint32_t cp, int64_t row, int col, uint8_t width, uint32_t link) {
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 256;
        uint32_t *c = realloc(t->cp, (size_t)cap * sizeof *c);
        if (c) t->cp = c;
        int64_t *r = realloc(t->row, (size_t)cap * sizeof *r);
        if (r) t->row = r;
        int *x = realloc(t->col, (size_t)cap * sizeof *x);
        if (x) t->col = x;
        uint8_t *w = realloc(t->width, (size_t)cap);
        if (w) t->width = w;
        uint32_t *l = realloc(t->link, (size_t)cap * sizeof *l);
        if (l) t->link = l;
        if (!c || !r || !x || !w || !l) return false;
        t->cap = cap;
    }
    t->cp[t->n] = cp; t->row[t->n] = row; t->col[t->n] = col; t->width[t->n] = width; t->link[t->n] = link;
    t->n++;
    return true;
}
static bool blank(const struct vt_cell *c) { return c->cp == ' ' && !c->flags; }
static uint32_t base_of(const struct vt *v, const struct vt_cell *c) {
    if (c->flags & VT_CELL_CLUSTER && c->cp < v->nclusters) return v->clusters[c->cp][0];
    return c->cp;
}

bool term_text_line(const struct vt *v, int64_t id, struct term_text *t) {
    bool wrapped;
    t->n = 0;
    if (!vt_row_cells(v, id, &wrapped)) return false;
    int64_t first = id, last = id;
    for (;;) {                                   /* back to the row the line began on */
        bool w;
        if (!vt_row_cells(v, first - 1, &w) || !w) break;
        first--;
    }
    while (wrapped) {                            /* on to the row it ended on */
        if (!vt_row_cells(v, last + 1, &wrapped)) break;
        last++;
    }
    t->first = first; t->last = last;
    for (int64_t r = first; r <= last; r++) {
        bool w;
        const struct vt_cell *cells = vt_row_cells(v, r, &w);
        if (!cells) break;
        int end = v->cols;
        if (!w) while (end > 0 && blank(&cells[end - 1])) end--;
        for (int x = 0; x < end; x++) {
            const struct vt_cell *c = &cells[x];
            if (!c->width) continue;
            if (!push(t, base_of(v, c), r, x, c->width, vt_style_of(v, c)->link)) return false;
        }
    }
    return true;
}
void term_text_free(struct term_text *t) {
    free(t->cp); free(t->row); free(t->col); free(t->width); free(t->link);
    memset(t, 0, sizeof *t);
}

static int cmp(struct term_point a, struct term_point b) {
    return a.row != b.row ? (a.row < b.row ? -1 : 1) : (a.col != b.col ? (a.col < b.col ? -1 : 1) : 0);
}
static struct term_point at(const struct term_text *t, int i) { struct term_point p = { t->row[i], t->col[i] }; return p; }
static struct term_point after(const struct term_text *t, int i) {
    struct term_point p = { t->row[i], t->col[i] + t->width[i] }; return p;
}
/* The character whose cell covers p, or -1. */
static int covering(const struct term_text *t, struct term_point p) {
    for (int i = 0; i < t->n; i++)
        if (t->row[i] == p.row && t->col[i] <= p.col && p.col < t->col[i] + t->width[i]) return i;
    return -1;
}

/* --- selection ---------------------------------------------------------------------- */

static bool word_char(uint32_t c) {
    if (c <= ' ' || c == 0x7f) return false;
    if (c < 0x80) return !strchr("()[]{}<>\"'`|;,&", (int)c);
    if (c >= 0x2500 && c <= 0x259f) return false;           /* box drawing, blocks */
    if (c >= 0xe0a0 && c <= 0xe0d7) return false;           /* Powerline separators */
    return true;
}
/* The word holding cell p -- or its run of blanks, or the one character
 * that is neither -- as [lo, hi); just the cell when p holds no text. */
static void word_at(const struct vt *v, struct term_point p, struct term_point *lo, struct term_point *hi) {
    struct term_text t = {0};
    *lo = p; *hi = p; hi->col = p.col + 1;
    if (term_text_line(v, p.row, &t)) {
        int i = covering(&t, p);
        if (i >= 0) {
            int a = i, b = i + 1;
            bool word = word_char(t.cp[i]), space = t.cp[i] == ' ';
            if (word) { while (a > 0 && word_char(t.cp[a - 1])) a--; while (b < t.n && word_char(t.cp[b])) b++; }
            else if (space) { while (a > 0 && t.cp[a - 1] == ' ') a--; while (b < t.n && t.cp[b] == ' ') b++; }
            *lo = at(&t, a); *hi = after(&t, b - 1);
        }
    }
    term_text_free(&t);
}
static void line_at(const struct vt *v, struct term_point p, struct term_point *lo, struct term_point *hi) {
    struct term_text t = {0};
    lo->row = hi->row = p.row; lo->col = 0; hi->col = v->cols;
    if (term_text_line(v, p.row, &t)) { lo->row = t.first; hi->row = t.last; }
    term_text_free(&t);
}
static void unit_at(enum term_select_mode mode, const struct vt *v, struct term_point p,
                    struct term_point *lo, struct term_point *hi) {
    if (mode == TERM_SELECT_WORDS) word_at(v, p, lo, hi);
    else if (mode == TERM_SELECT_LINES) line_at(v, p, lo, hi);
    else { *lo = p; *hi = p; }
}
void term_select_start(struct term_selection *s, const struct vt *v, struct term_point p, enum term_select_mode mode) {
    s->active = true; s->mode = mode;
    s->anchor = s->head = p;
    unit_at(mode, v, p, &s->anchor_lo, &s->anchor_hi);
    s->head_lo = s->anchor_lo; s->head_hi = s->anchor_hi;
}
void term_select_extend(struct term_selection *s, const struct vt *v, struct term_point p) {
    if (!s->active) return;
    s->head = p;
    unit_at(s->mode, v, p, &s->head_lo, &s->head_hi);
}
void term_select_clear(struct term_selection *s) { s->active = false; }
void term_select_all(struct term_selection *s, const struct vt *v) {
    s->active = true; s->mode = TERM_SELECT_CHARS;
    s->anchor.row = vt_row_id(v, 0, v->alt ? 0 : v->history_rows); s->anchor.col = 0;
    s->head.row = vt_row_id(v, v->rows - 1, 0); s->head.col = v->cols;
    s->anchor_lo = s->anchor_hi = s->head_lo = s->head_hi = s->anchor;
}
void term_select_bounds(const struct term_selection *s, struct term_point *start, struct term_point *end) {
    switch (s->mode) {
    case TERM_SELECT_BLOCK:
        start->row = s->anchor.row < s->head.row ? s->anchor.row : s->head.row;
        end->row = s->anchor.row < s->head.row ? s->head.row : s->anchor.row;
        start->col = s->anchor.col < s->head.col ? s->anchor.col : s->head.col;
        end->col = s->anchor.col < s->head.col ? s->head.col : s->anchor.col;
        break;
    case TERM_SELECT_CHARS:
        *start = cmp(s->anchor, s->head) <= 0 ? s->anchor : s->head;
        *end = cmp(s->anchor, s->head) <= 0 ? s->head : s->anchor;
        break;
    default:
        *start = cmp(s->anchor_lo, s->head_lo) <= 0 ? s->anchor_lo : s->head_lo;
        *end = cmp(s->anchor_hi, s->head_hi) >= 0 ? s->anchor_hi : s->head_hi;
        break;
    }
}
bool term_select_span(const struct term_selection *s, const struct vt *v, int64_t id, int cols, int *x0, int *x1) {
    (void)v;
    if (!s->active) return false;
    struct term_point a, b;
    term_select_bounds(s, &a, &b);
    if (id < a.row || id > b.row) return false;
    if (s->mode == TERM_SELECT_BLOCK) { *x0 = a.col; *x1 = b.col; }
    else { *x0 = id == a.row ? a.col : 0; *x1 = id == b.row ? b.col : cols; }
    if (*x0 < 0) *x0 = 0;
    if (*x1 > cols) *x1 = cols;
    return *x0 < *x1;
}

static size_t put(char *out, size_t cap, size_t n, const char *s, size_t k) {
    if (n + k + 1 > cap) k = n + 1 < cap ? cap - n - 1 : 0;
    memcpy(out + n, s, k);
    return n + k;
}
size_t term_select_text(const struct term_selection *s, const struct vt *v, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    if (!s->active) return 0;
    struct term_point a, b;
    term_select_bounds(s, &a, &b);
    for (int64_t r = a.row; r <= b.row; r++) {
        int x0, x1;
        bool w;
        const struct vt_cell *cells = vt_row_cells(v, r, &w);
        size_t row_start = n;
        if (cells && term_select_span(s, v, r, v->cols, &x0, &x1)) {
            for (int x = x0; x < x1; x++) {
                const struct vt_cell *c = &cells[x];
                if (!c->width) continue;
                char text[64]; size_t k;
                if (vt_style_of(v, c)->attrs & VT_HIDDEN) { text[0] = ' '; k = 1; }
                else k = vt_cell_text(v, c, text);
                n = put(out, cap, n, text, k);
            }
        }
        bool joined = s->mode != TERM_SELECT_BLOCK && w && cells && r < b.row;
        if (!joined) {
            while (n > row_start && out[n - 1] == ' ') n--;      /* a line's trailing blanks are not text */
            if (r < b.row) n = put(out, cap, n, "\n", 1);
        }
    }
    while (n && out[n - 1] == '\n') n--;
    out[n] = 0;
    return n;
}

/* --- find ------------------------------------------------------------------------ */

static uint32_t fold(uint32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int decode(const char *s, uint32_t *cp, int max) {
    int n = 0;
    const unsigned char *u = (const unsigned char *)s;
    while (*u && n < max) {
        uint32_t c; int k;
        if (u[0] < 0x80) { c = u[0]; k = 1; }
        else if ((u[0] & 0xe0) == 0xc0 && u[1]) { c = (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f); k = 2; }
        else if ((u[0] & 0xf0) == 0xe0 && u[1] && u[2]) { c = (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f); k = 3; }
        else if ((u[0] & 0xf8) == 0xf0 && u[1] && u[2] && u[3]) {
            c = (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f); k = 4;
        } else { c = 0xfffd; k = 1; }
        cp[n++] = fold(c); u += k;
    }
    return n;
}
static bool match_at(const struct term_text *t, int i, const uint32_t *needle, int m) {
    if (i + m > t->n) return false;
    for (int k = 0; k < m; k++) if (fold(t->cp[i + k]) != needle[k]) return false;
    return true;
}
static int64_t lowest(const struct vt *v) { return vt_row_id(v, 0, v->alt ? 0 : v->history_rows); }
static int64_t highest(const struct vt *v) { return vt_row_id(v, v->rows - 1, 0); }

bool term_find(const struct vt *v, const char *text, struct term_point from, bool backward, struct term_match *out) {
    uint32_t needle[256];
    int m = decode(text, needle, 256);
    if (!m) return false;
    int64_t lo = lowest(v), hi = highest(v), r = from.row;
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    struct term_text t = {0};
    bool first = true, found = false;
    while (r >= lo && r <= hi && term_text_line(v, r, &t)) {
        int pick = -1;
        for (int i = 0; i + m <= t.n; i++) {
            if (!match_at(&t, i, needle, m)) continue;
            int c = cmp(at(&t, i), from);
            if (backward) { if (!first || c < 0) pick = i; }
            else if (!first || c > 0) { pick = i; break; }
        }
        if (pick >= 0) {
            out->start = at(&t, pick); out->end = after(&t, pick + m - 1);
            found = true;
            break;
        }
        first = false;
        r = backward ? t.first - 1 : t.last + 1;
    }
    term_text_free(&t);
    return found;
}
int term_find_spans(const struct vt *v, const char *text, int64_t id, int *x0, int *x1, int max) {
    uint32_t needle[256];
    int m = decode(text, needle, 256), n = 0;
    struct term_text t = {0};
    if (!m || !term_text_line(v, id, &t)) { term_text_free(&t); return 0; }
    for (int i = 0; i + m <= t.n && n < max; i++) {
        if (!match_at(&t, i, needle, m)) continue;
        int a = -1, b = -1;
        for (int k = i; k < i + m; k++) if (t.row[k] == id) { if (a < 0) a = k; b = k; }
        if (a >= 0) { x0[n] = t.col[a]; x1[n] = t.col[b] + t.width[b]; n++; }
        i += m - 1;
    }
    term_text_free(&t);
    return n;
}

/* --- links --------------------------------------------------------------------- */

static bool alnum(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
static bool url_char(uint32_t c) {
    if (c <= ' ' || c == 0x7f) return false;
    if (c < 0x80) return !strchr("<>\"'`\\^|", (int)c);
    if (c >= 0x2500 && c <= 0x259f) return false;
    if (c >= 0xe000 && c <= 0xf8ff) return false;          /* icons */
    return true;
}
/* Where a URL starting at k ends (exclusive), or k when none starts there. */
static int url_end(const struct term_text *t, int k, int *skip_prefix_www) {
    int n = t->n, i = k;
    *skip_prefix_www = 0;
    if (k > 0 && (alnum(t->cp[k - 1]) || t->cp[k - 1] == '.' || t->cp[k - 1] == '/')) return k;
    static const char www[] = "www.", mailto[] = "mailto:";
    bool is_www = true, is_mail = true;
    for (int j = 0; j < 4; j++) if (k + j >= n || fold(t->cp[k + j]) != (uint32_t)www[j]) is_www = false;
    for (int j = 0; j < 7; j++) if (k + j >= n || fold(t->cp[k + j]) != (uint32_t)mailto[j]) is_mail = false;
    int body;
    if (is_www) { body = k + 4; *skip_prefix_www = 1; }
    else if (is_mail) body = k + 7;
    else {
        if (!((t->cp[i] >= 'a' && t->cp[i] <= 'z') || (t->cp[i] >= 'A' && t->cp[i] <= 'Z'))) return k;
        while (i < n && (alnum(t->cp[i]) || t->cp[i] == '+' || t->cp[i] == '-' || t->cp[i] == '.')) i++;
        if (i + 3 > n || t->cp[i] != ':' || t->cp[i + 1] != '/' || t->cp[i + 2] != '/') return k;
        body = i + 3;
    }
    int e = body;
    while (e < n && url_char(t->cp[e])) e++;
    /* trailing punctuation belongs to the sentence; an unmatched closer too */
    for (;;) {
        if (e > body && strchr(".,;:!?", (int)(t->cp[e - 1] < 0x80 ? t->cp[e - 1] : 'x'))) { e--; continue; }
        static const char close[] = ")]}", open[] = "([{";
        bool trimmed = false;
        for (int q = 0; q < 3 && e > body; q++) if (t->cp[e - 1] == (uint32_t)close[q]) {
            int depth = 0;
            for (int j = body; j < e; j++) depth += t->cp[j] == (uint32_t)open[q] ? 1 : t->cp[j] == (uint32_t)close[q] ? -1 : 0;
            if (depth < 0) { e--; trimmed = true; }
        }
        if (!trimmed) break;
    }
    return e > body ? e : k;
}
static size_t utf8(uint32_t c, char *o) { return vt_encode(c, o); }

bool term_link_at(const struct vt *v, struct term_point p, char *url, size_t cap, struct term_match *where) {
    struct term_text t = {0};
    bool found = false;
    if (!cap || !term_text_line(v, p.row, &t)) { term_text_free(&t); return false; }
    int i = covering(&t, p);
    if (i >= 0 && t.link[i]) {
        uint32_t link = t.link[i];
        int a = i, b = i + 1;
        while (a > 0 && t.link[a - 1] == link) a--;
        while (b < t.n && t.link[b] == link) b++;
        const char *uri = vt_link_uri(v, link);
        size_t k = uri ? strlen(uri) : 0;
        if (k >= cap) k = cap - 1;
        memcpy(url, uri ? uri : "", k); url[k] = 0;
        where->start = at(&t, a); where->end = after(&t, b - 1);
        found = k > 0;
    } else if (i >= 0) {
        for (int k = 0; k < t.n && !found; ) {
            int www, e = url_end(&t, k, &www);
            if (e == k) { k++; continue; }
            if (k <= i && i < e) {
                size_t n = 0;
                if (www) { memcpy(url, "http://", cap > 8 ? 7 : 0); n = cap > 8 ? 7 : 0; }
                for (int j = k; j < e; j++) {
                    char o[4]; size_t len = utf8(t.cp[j], o);
                    if (n + len + 1 > cap) break;
                    memcpy(url + n, o, len); n += len;
                }
                url[n] = 0;
                where->start = at(&t, k); where->end = after(&t, e - 1);
                found = true;
            }
            k = e;
        }
    }
    term_text_free(&t);
    return found;
}
