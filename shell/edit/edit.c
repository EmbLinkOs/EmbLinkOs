/* ==========================================================================
 * edit.c -- the shell's line editor. See edit.h for the contract.
 *
 * Three layers, top to bottom:
 *   decode   terminal bytes -> keys (ANSI sequences, UTF-8, bracketed paste,
 *            in-band resize reports; or the console's private bytes)
 *   handle   keys -> edits of `line` (a UTF-8 byte buffer with a cursor),
 *            history, search, completion
 *   render   the whole state -> bytes that redraw it (EDIT_VT), or the
 *            difference from what the console shows (EDIT_DUMB)
 *
 * The VT renderer always redraws from the prompt's first row: up to it,
 * clear to the end of the screen, prompt, text, then any menu or search line,
 * then back to the cursor -- relative motion only, so it works wherever on the
 * screen the prompt is and whatever scrolled. One write per feed keeps the
 * terminal from ever painting a half-drawn line.
 * ========================================================================== */
#include "edit/edit.h"
#include "edit/wcwidth.h"
#include "hist/hist.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- growable bytes -------------------------------------------------------- */

struct buf { char *p; size_t n, cap; };

static bool buf_reserve(struct buf *b, size_t more) {
    if (b->n + more + 1 <= b->cap) return true;
    size_t want = b->cap ? b->cap : 64;
    while (want < b->n + more + 1) want *= 2;
    char *grown = realloc(b->p, want);
    if (!grown) return false;
    b->p = grown;
    b->cap = want;
    return true;
}
static void buf_add(struct buf *b, const char *s, size_t n) {
    if (!n || !buf_reserve(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void buf_str(struct buf *b, const char *s) { if (s) buf_add(b, s, strlen(s)); }
static void buf_fmt(struct buf *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
static void buf_set(struct buf *b, const char *s, size_t n) {
    b->n = 0;
    if (!buf_reserve(b, n)) return;
    if (n) memcpy(b->p, s, n);
    b->n = n;
    b->p[n] = '\0';
}
static const char *buf_text(const struct buf *b) { return b->p ? b->p : ""; }
static char *dup_or_null(const char *s) { return s ? strdup(s) : 0; }

/* --- keys ------------------------------------------------------------------ */

enum key_kind {
    K_NONE, K_TEXT, K_CTRL, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DELETE,
    K_INSERT, K_PGUP, K_PGDN, K_BACKSPACE, K_ENTER, K_TAB, K_BACKTAB, K_ESCAPE, K_RESIZE,
    K_FKEY, K_PASTE_TEXT,
};
#define M_SHIFT 1u
#define M_ALT   2u
#define M_CTRL  4u

struct key {
    enum key_kind kind;
    uint32_t ch;        /* K_TEXT / K_PASTE_TEXT: the codepoint; K_CTRL: the letter */
    unsigned mods;
    int rows, cols;     /* K_RESIZE */
};

/* --- the editor ------------------------------------------------------------ */

#define KILLS 8
#define UNDOS 100

struct snapshot { char *text; size_t cur; };

enum last_action { A_OTHER, A_INSERT, A_KILL, A_YANK, A_PREFIX };

struct editor {
    struct edit_host host;
    enum edit_mode mode;
    char *mark, *prompt, *more, *after, *home, *right, *transient;
    int cols;

    struct buf line;          /* the text being edited, NUL-terminated */
    size_t cur;               /* the cursor, a byte offset on a character boundary */
    struct buf out;           /* bytes for the terminal, written once per feed */
    enum edit_result result;
    struct buf accepted;

    /* decoder */
    int dstate;               /* 0 text, 1 after ESC, 2 CSI, 3 SS3 */
    char seq[32];
    int seqn;
    char utf[4];
    int utfn, utfneed;
    bool paste;               /* between ESC[200~ and ESC[201~ */
    bool paste_cr;            /* the last pasted byte was CR: a following LF is the same newline */
    bool awaiting_first_report;

    /* the VT screen */
    bool fresh;               /* nothing drawn: the cursor is at the prompt's origin */
    int cursor_row;           /* rows below the origin that the cursor is on */

    /* the console screen */
    struct buf shown;         /* what the console shows of the editable segment */
    size_t shown_cur;         /* the console cursor, as a byte offset into `shown` */
    size_t shown_base;        /* where that segment starts in `line` */

    /* history */
    size_t hpos;              /* hist_count() = the new line being typed */
    char *saved;              /* that new line, while browsing away from it */
    struct buf prefix;        /* PgUp/PgDn: what the found lines must begin with */

    /* incremental search */
    bool searching, sfail;
    struct buf query;
    size_t smatch;            /* the matched entry + 1, or 0 for none */
    char *sorig; size_t sorig_cur;

    /* kill ring and undo */
    char *kills[KILLS];
    int nkills, killtop;
    size_t yank_at, yank_len;
    enum last_action last;
    struct snapshot undo[UNDOS];
    int nundo;

    /* the suggestion from history (zsh-autosuggestions' POSTDISPLAY): what the
     * newest history line that starts with the whole line adds to it */
    struct buf suggestion;
    bool accepting;           /* the line is done: drawn the last time */

    /* completion */
    struct edit_matches m;
    bool menu;
    int sel;                  /* -1: the menu is shown but nothing is chosen */
    char *menu_word;          /* what was typed before choosing, for Escape */
    size_t menu_start, menu_len;
};

/* --- completion candidates --------------------------------------------------- */

bool edit_matches_add(struct edit_matches *m, const char *insert, const char *show, bool dir) {
    if (m->n == m->cap) {
        size_t want = m->cap ? m->cap * 2 : 32;
        struct edit_item *grown = realloc(m->items, want * sizeof *grown);
        if (!grown) return false;
        m->items = grown;
        m->cap = want;
    }
    struct edit_item *it = &m->items[m->n];
    it->insert = strdup(insert);
    it->show = strdup(show ? show : insert);
    it->dir = dir;
    if (!it->insert || !it->show) { free(it->insert); free(it->show); return false; }
    m->n++;
    return true;
}
static void matches_clear(struct edit_matches *m) {
    for (size_t i = 0; i < m->n; i++) { free(m->items[i].insert); free(m->items[i].show); }
    free(m->items);
    memset(m, 0, sizeof *m);
}

/* --- characters -------------------------------------------------------------- */

static uint32_t cp_at(const struct editor *e, size_t i, int *len) {
    uint32_t cp;
    *len = utf8_decode(e->line.p + i, (int)(e->line.n - i), &cp);
    return cp;
}
/* One step right: a character plus every combining mark that joins it, so the
 * cursor never stops between a letter and its accent. */
static size_t next_char(const struct editor *e, size_t i) {
    if (i >= e->line.n) return e->line.n;
    int len;
    cp_at(e, i, &len);
    i += (size_t)len;
    while (i < e->line.n) {
        uint32_t cp = cp_at(e, i, &len);
        if (wcwidth_cp(cp) != 0) break;
        i += (size_t)len;
    }
    return i;
}
static size_t prev_char(const struct editor *e, size_t i) {
    if (i == 0) return 0;
    for (;;) {
        do { i--; } while (i > 0 && ((unsigned char)e->line.p[i] & 0xc0) == 0x80);
        int len;
        uint32_t cp = cp_at(e, i, &len);
        if (i == 0 || wcwidth_cp(cp) != 0) return i;
    }
}
static bool is_word_byte(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n'; }
static size_t word_left(const struct editor *e, size_t i) {
    while (i > 0 && !is_word_byte((unsigned char)e->line.p[i - 1])) i--;
    while (i > 0 && is_word_byte((unsigned char)e->line.p[i - 1])) i--;
    return i;
}
static size_t word_right(const struct editor *e, size_t i) {
    while (i < e->line.n && !is_word_byte((unsigned char)e->line.p[i])) i++;
    while (i < e->line.n && is_word_byte((unsigned char)e->line.p[i])) i++;
    return i;
}
static size_t line_start(const struct editor *e, size_t i) {
    while (i > 0 && e->line.p[i - 1] != '\n') i--;
    return i;
}
static size_t line_end(const struct editor *e, size_t i) {
    while (i < e->line.n && e->line.p[i] != '\n') i++;
    return i;
}
/* Cells from the start of the logical line to byte i. */
static int column_of(const struct editor *e, size_t i) {
    int col = 0;
    for (size_t j = line_start(e, i); j < i;) {
        int len;
        uint32_t cp = cp_at(e, j, &len);
        int w = wcwidth_cp(cp);
        col += w > 0 ? w : 0;
        j += (size_t)len;
    }
    return col;
}
/* The byte in the logical line starting at `start` closest to column `col`. */
static size_t at_column(const struct editor *e, size_t start, int col) {
    size_t i = start;
    int c = 0;
    while (i < e->line.n && e->line.p[i] != '\n') {
        int len;
        uint32_t cp = cp_at(e, i, &len);
        int w = wcwidth_cp(cp);
        if (w < 0) w = 0;
        if (c + w > col) break;
        c += w;
        i = next_char(e, i);
    }
    return i;
}

/* --- edits ------------------------------------------------------------------ */

static void snapshot(struct editor *e) {
    if (e->nundo == UNDOS) {
        free(e->undo[0].text);
        memmove(e->undo, e->undo + 1, (UNDOS - 1) * sizeof e->undo[0]);
        e->nundo--;
    }
    char *copy = strdup(buf_text(&e->line));
    if (!copy) return;
    e->undo[e->nundo].text = copy;
    e->undo[e->nundo].cur = e->cur;
    e->nundo++;
}
static void set_line(struct editor *e, const char *text, size_t cur) {
    buf_set(&e->line, text, strlen(text));
    e->cur = cur > e->line.n ? e->line.n : cur;
}
static void insert(struct editor *e, const char *s, size_t n) {
    if (!n || !buf_reserve(&e->line, n)) return;
    memmove(e->line.p + e->cur + n, e->line.p + e->cur, e->line.n - e->cur + 1);
    memcpy(e->line.p + e->cur, s, n);
    e->line.n += n;
    e->cur += n;
}
static void erase(struct editor *e, size_t a, size_t b) {
    if (b <= a) return;
    memmove(e->line.p + a, e->line.p + b, e->line.n - b + 1);
    e->line.n -= b - a;
    if (e->cur >= b) e->cur -= b - a;
    else if (e->cur > a) e->cur = a;
}
/* Kills go to the ring; consecutive kills grow one entry, as in emacs, so
 * Ctrl+W Ctrl+W yanks back as one piece. */
static void kill(struct editor *e, size_t a, size_t b, bool before) {
    if (b <= a) return;
    size_t n = b - a;
    if (e->last == A_KILL && e->nkills) {
        char *old = e->kills[e->killtop];
        size_t on = strlen(old);
        char *joined = malloc(on + n + 1);
        if (joined) {
            if (before) { memcpy(joined, e->line.p + a, n); memcpy(joined + n, old, on); }
            else { memcpy(joined, old, on); memcpy(joined + on, e->line.p + a, n); }
            joined[on + n] = '\0';
            free(old);
            e->kills[e->killtop] = joined;
        }
    } else {
        char *k = malloc(n + 1);
        if (k) {
            memcpy(k, e->line.p + a, n);
            k[n] = '\0';
            e->killtop = (e->killtop + 1) % KILLS;
            free(e->kills[e->killtop]);
            e->kills[e->killtop] = k;
            if (e->nkills < KILLS) e->nkills++;
        }
    }
    erase(e, a, b);
    e->last = A_KILL;
}

/* --- history ----------------------------------------------------------------- */

static void history_show(struct editor *e, size_t pos, size_t cur) {
    if (e->hpos == hist_count() && pos != e->hpos) {
        free(e->saved);
        e->saved = strdup(buf_text(&e->line));
    }
    e->hpos = pos;
    const char *text = pos == hist_count() ? (e->saved ? e->saved : "") : hist_get(pos);
    set_line(e, text ? text : "", cur == (size_t)-1 ? (size_t)-1 : cur);
}
static void history_step(struct editor *e, int dir) {
    if (dir < 0 && e->hpos > 0) history_show(e, e->hpos - 1, (size_t)-1);
    else if (dir > 0 && e->hpos < hist_count()) history_show(e, e->hpos + 1, (size_t)-1);
}
/* PgUp/PgDn: the nearest entry that begins with what is before the cursor. */
static void history_prefix(struct editor *e, int dir) {
    if (e->last != A_PREFIX) buf_set(&e->prefix, e->line.p ? e->line.p : "", e->cur);
    size_t n = e->prefix.n, count = hist_count();
    for (size_t pos = e->hpos;;) {
        if (dir < 0) { if (pos == 0) break; pos--; }
        else { if (pos >= count) break; pos++; }
        const char *h = pos == count ? (e->saved ? e->saved : "") : hist_get(pos);
        if (!h) break;
        if ((pos == count || strncmp(h, buf_text(&e->prefix), n) == 0) && strcmp(h, buf_text(&e->line)) != 0) {
            history_show(e, pos, n);
            break;
        }
    }
    e->last = A_PREFIX;
}

/* --- incremental search -------------------------------------------------------- */

static bool search_from(struct editor *e, size_t start, int dir) {
    size_t count = hist_count();
    const char *q = buf_text(&e->query);
    for (size_t pos = start; pos < count; pos = dir < 0 ? pos - 1 : pos + 1) {
        const char *h = hist_get(pos);
        const char *hit = h ? strstr(h, q) : 0;
        if (hit) {
            e->smatch = pos + 1;
            set_line(e, h, (size_t)(hit - h));
            e->hpos = pos;
            e->sfail = false;
            return true;
        }
        if (dir < 0 && pos == 0) break;
    }
    e->sfail = true;
    return false;
}
static void search_begin(struct editor *e) {
    e->searching = true;
    e->sfail = false;
    buf_set(&e->query, "", 0);
    e->smatch = 0;
    free(e->sorig);
    e->sorig = strdup(buf_text(&e->line));
    e->sorig_cur = e->cur;
}
static void search_end(struct editor *e, bool keep) {
    e->searching = false;
    if (!keep && e->sorig) set_line(e, e->sorig, e->sorig_cur);
    free(e->sorig);
    e->sorig = 0;
}

/* --- completion ------------------------------------------------------------------ */

static void menu_close(struct editor *e) {
    e->menu = false;
    free(e->menu_word);
    e->menu_word = 0;
    matches_clear(&e->m);
}
static void replace(struct editor *e, size_t start, size_t len, const char *with) {
    erase(e, start, start + len);
    e->cur = start;
    insert(e, with, strlen(with));
}
static void menu_choose(struct editor *e, int sel) {
    if (!e->m.n) return;
    e->sel = (sel % (int)e->m.n + (int)e->m.n) % (int)e->m.n;
    const char *with = e->m.items[e->sel].insert;
    replace(e, e->menu_start, e->menu_len, with);
    e->menu_len = strlen(with);
}
static void complete(struct editor *e, bool backwards) {
    if (!e->host.complete) return;
    if (e->menu && e->m.n) { menu_choose(e, e->sel < 0 ? (backwards ? -1 : 0) : e->sel + (backwards ? -1 : 1)); return; }
    matches_clear(&e->m);
    e->host.complete(e->host.ctx, buf_text(&e->line), e->cur, &e->m);
    if (!e->m.n || e->m.start > e->cur) { matches_clear(&e->m); return; }
    size_t word = e->cur - e->m.start;
    snapshot(e);
    if (e->m.n == 1) {
        replace(e, e->m.start, word, e->m.items[0].insert);
        if (!e->m.items[0].dir) insert(e, " ", 1);
        matches_clear(&e->m);
        return;
    }
    /* The longest prefix every candidate shares, cut back to a whole character. */
    const char *first = e->m.items[0].insert;
    size_t common = strlen(first);
    for (size_t i = 1; i < e->m.n; i++) {
        size_t j = 0;
        while (j < common && e->m.items[i].insert[j] == first[j]) j++;
        common = j;
    }
    while (common && ((unsigned char)first[common] & 0xc0) == 0x80) common--;
    if (common > word || (common == word && strncmp(first, e->line.p + e->m.start, word) != 0)) {
        char *part = malloc(common + 1);
        if (part) {
            memcpy(part, first, common);
            part[common] = '\0';
            replace(e, e->m.start, word, part);
            free(part);
        }
        matches_clear(&e->m);
        return;
    }
    /* No progress to be made by typing for them: show them all. The console
     * cannot hold a menu, so it gets the list once and a fresh prompt, as
     * bash does; a terminal gets the menu to pick from. */
    if (e->mode == EDIT_DUMB) {
        buf_str(&e->out, "\r\n");
        for (size_t i = 0; i < e->m.n; i++) { buf_str(&e->out, e->m.items[i].show); buf_str(&e->out, "  "); }
        buf_str(&e->out, "\r\n");
        e->fresh = true;
        matches_clear(&e->m);
        return;
    }
    e->menu = true;
    e->sel = -1;
    e->menu_start = e->m.start;
    e->menu_len = word;
    free(e->menu_word);
    e->menu_word = malloc(word + 1);
    if (e->menu_word) { memcpy(e->menu_word, e->line.p + e->m.start, word); e->menu_word[word] = '\0'; }
}

/* --- width of what is drawn ---------------------------------------------------------- */

struct pos { int row, col; };

/* Advance over printable text, skipping escape sequences (they take no cells).
 * A prompt of several lines (Powerlevel10k's two) has \r\n between them. */
static void layout(const char *s, size_t n, struct pos *p, int cols) {
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r') { p->col = 0; i++; continue; }
        if (c == '\n') { p->row++; p->col = 0; i++; continue; }
        if (c == 0x1b && i + 1 < n && (s[i + 1] == '[' || s[i + 1] == ']')) {
            bool osc = s[i + 1] == ']';
            i += 2;
            while (i < n) {
                unsigned char d = (unsigned char)s[i++];
                if (osc ? (d == 7 || (d == 0x1b && i < n && s[i] == '\\' && ++i)) : (d >= 0x40 && d <= 0x7e)) break;
            }
            continue;
        }
        uint32_t cp;
        int len = utf8_decode(s + i, (int)(n - i), &cp);
        i += (size_t)len;
        int w = wcwidth_cp(cp);
        if (w <= 0) continue;
        if (p->col + w > cols) { p->row++; p->col = 0; }
        p->col += w;
    }
}
static int text_width(const char *s) {
    struct pos p = { 0, 0 };
    layout(s, strlen(s), &p, 1 << 20);
    return p.col;
}
static struct pos settled(struct pos p, int cols) {
    if (p.col >= cols) { p.row++; p.col = 0; }
    return p;
}

/* --- the VT renderer ------------------------------------------------------------- */

struct grid { int colw, ncols, nrows, first, shown; };
static struct grid menu_grid(const struct editor *e) {
    struct grid g = { 0, 1, 0, 0, 0 };
    int widest = 1;
    for (size_t i = 0; i < e->m.n; i++) {
        int w = text_width(e->m.items[i].show);
        if (w > widest) widest = w;
    }
    g.colw = widest + 2;
    g.ncols = (e->cols - 1) / g.colw;
    if (g.ncols < 1) g.ncols = 1;
    g.nrows = (int)((e->m.n + (size_t)g.ncols - 1) / (size_t)g.ncols);
    g.shown = g.nrows < 10 ? g.nrows : 10;
    if (e->sel >= 0) {
        int row = e->sel % g.nrows;
        if (row >= g.shown) g.first = row - g.shown + 1;
    }
    return g;
}
/* One cell-limited piece of text: what fits in `room` cells. */
static void clipped(struct buf *o, const char *s, int room) {
    int used = 0;
    for (size_t i = 0, n = strlen(s); i < n;) {
        uint32_t cp;
        int len = utf8_decode(s + i, (int)(n - i), &cp);
        int w = wcwidth_cp(cp);
        if (w > 0 && used + w > room) break;
        if (w >= 0) buf_add(o, s + i, (size_t)len);
        if (w > 0) used += w;
        i += (size_t)len;
    }
    for (; used < room; used++) buf_add(o, " ", 1);
}
/* A highlighter's style (edit.h) as SGR, from the terminal's own pen. */
static void style_sgr(struct buf *o, unsigned char s) {
    int fg = (s & EDIT_FG_MASK) - 1;
    buf_str(o, "\033[0");
    if (s & EDIT_BOLD) buf_str(o, ";1");
    if (s & EDIT_UNDERLINE) buf_str(o, ";4");
    if (fg >= 0 && fg < 8) buf_fmt(o, ";%d", 30 + fg);
    else if (fg >= 8) buf_fmt(o, ";38;5;%d", fg);
    buf_str(o, "m");
}
static bool suggesting(const struct editor *e) { return e->suggestion.n && !e->searching && !e->menu; }

static void render_vt(struct editor *e) {
    struct buf *o = &e->out;
    int cols = e->cols;
    unsigned char *style = NULL;
    if (e->host.highlight && e->line.n && (style = calloc(e->line.n, 1)))
        e->host.highlight(e->host.ctx, e->line.p, e->line.n, e->accepting, style);
    int pen = 0;
    if (e->fresh) buf_str(o, e->mark);
    else if (e->home) buf_str(o, e->home);
    else {
        if (e->cursor_row) buf_fmt(o, "\033[%dA", e->cursor_row);
        buf_str(o, "\r");
    }
    buf_str(o, "\033[J");
    struct pos p = { 0, 0 }, cursor = { 0, 0 };
    buf_str(o, e->prompt);
    if (e->prompt) layout(e->prompt, strlen(e->prompt), &p, cols);
    /* The right prompt, against the edge one column in -- while the typing
     * on this row leaves a column between them. It is drawn now, before the
     * typing, and the cursor comes back to where the typing starts. */
    if (e->right && e->right[0] && !e->searching) {
        struct pos q = p;
        bool fits = true, ended = false;
        for (size_t i = 0; i < e->line.n && fits;) {
            if (e->line.p[i] == '\n') { ended = true; break; }
            int len;
            int w = wcwidth_cp(cp_at(e, i, &len));
            if (w > 0) { if (q.col + w > cols) fits = false; else q.col += w; }
            i += (size_t)len;
        }
        if (!ended && suggesting(e)) layout(e->suggestion.p, strcspn(e->suggestion.p, "\n"), &q, 1 << 20);
        if (q.col > cols) fits = false;
        int rw = text_width(e->right), at = cols - 1 - rw;
        if (fits && rw > 0 && q.col + 1 <= at && at > p.col) {
            buf_fmt(o, "\033[%dC", at - p.col);
            buf_str(o, e->right);
            buf_str(o, "\r");
            if (p.col > 0) buf_fmt(o, "\033[%dC", p.col);
        }
    }
    if (e->fresh) buf_str(o, e->after);
    for (size_t i = 0; i <= e->line.n;) {
        if (i == e->cur) cursor = settled(p, cols);
        if (i == e->line.n) break;
        if (e->line.p[i] == '\n') {
            if (pen) { buf_str(o, "\033[0m"); pen = 0; }
            buf_str(o, "\r\n");
            p.row++;
            p.col = 0;
            buf_str(o, e->more);
            if (e->more) layout(e->more, strlen(e->more), &p, cols);
            i++;
            continue;
        }
        int len;
        uint32_t cp = cp_at(e, i, &len);
        int w = wcwidth_cp(cp);
        if (style && style[i] != pen) { style_sgr(o, style[i]); pen = style[i]; }
        if (w >= 0) {
            buf_add(o, e->line.p + i, (size_t)len);
            if (w > 0) {
                if (p.col + w > cols) { p.row++; p.col = 0; }
                p.col += w;
            }
        }
        i += (size_t)len;
    }
    if (pen) buf_str(o, "\033[0m");
    free(style);
    /* the suggestion, after the line, in its own colour */
    if (suggesting(e)) {
        style_sgr(o, EDIT_SUGGESTION);
        const char *t = e->suggestion.p;
        for (size_t i = 0; i < e->suggestion.n;) {
            if (t[i] == '\n') {
                buf_str(o, "\033[0m\r\n");
                p.row++;
                p.col = 0;
                buf_str(o, e->more);
                if (e->more) layout(e->more, strlen(e->more), &p, cols);
                style_sgr(o, EDIT_SUGGESTION);
                i++;
                continue;
            }
            uint32_t cp;
            int len = utf8_decode(t + i, (int)(e->suggestion.n - i), &cp);
            int w = wcwidth_cp(cp);
            if (w >= 0) {
                buf_add(o, t + i, (size_t)len);
                if (w > 0) {
                    if (p.col + w > cols) { p.row++; p.col = 0; }
                    p.col += w;
                }
            }
            i += (size_t)len;
        }
        buf_str(o, "\033[0m");
    }
    /* A line that's done leaves the cursor after everything drawn */
    if (e->accepting) cursor = settled(p, cols);
    /* A line that ends exactly at the right margin leaves the terminal
     * waiting to wrap; settle it, so the cursor is where the next row starts. */
    if (p.col >= cols) { buf_str(o, " \r"); p.row++; p.col = 0; }
    if (e->searching) {
        buf_str(o, "\r\n");
        p.row++;
        buf_str(o, e->sfail ? "\033[2;31m" : "\033[2m");
        struct buf status = { 0 };
        buf_str(&status, e->sfail ? "failing search: " : "search history: ");
        buf_str(&status, buf_text(&e->query));
        clipped(o, buf_text(&status), cols - 1);
        free(status.p);
        buf_str(o, "\033[0m\r");
        p.col = 0;
    }
    if (e->menu && e->m.n) {
        struct grid g = menu_grid(e);
        for (int r = g.first; r < g.first + g.shown; r++) {
            buf_str(o, "\r\n");
            p.row++;
            int used = 0;
            for (int c = 0; c < g.ncols; c++) {
                size_t i = (size_t)(c * g.nrows + r);
                if (i >= e->m.n || used + g.colw > cols - 1) break;
                const struct edit_item *it = &e->m.items[i];
                buf_str(o, (int)i == e->sel ? "\033[7m" : it->dir ? "\033[1;34m" : "");
                clipped(o, it->show, g.colw - 2);
                buf_str(o, (int)i == e->sel || it->dir ? "\033[0m  " : "  ");
                used += g.colw;
            }
            buf_str(o, "\r");
        }
        if (g.shown < g.nrows) {
            buf_str(o, "\r\n\033[2m");
            p.row++;
            char more[64];
            snprintf(more, sizeof more, "rows %d-%d of %d", g.first + 1, g.first + g.shown, g.nrows);
            clipped(o, more, cols - 1 < 20 ? cols - 1 : 20);
            buf_str(o, "\033[0m\r");
        }
        p.col = 0;
    }
    int up = p.row - cursor.row;
    if (up > 0) buf_fmt(o, "\033[%dA", up);
    buf_str(o, "\r");
    if (cursor.col > 0) buf_fmt(o, "\033[%dC", cursor.col);
    e->cursor_row = cursor.row;
    e->fresh = false;
}

/* --- the console renderer --------------------------------------------------------
 * The kernel console knows backspace, CR and LF and nothing else, so the line is
 * kept in step by difference: back up to where old and new first differ, write
 * the new tail, blank what is left of the old one, back up to the cursor. Only
 * the last line of a multi-line program is editable here. */

static int cells(const char *s, size_t n) {
    struct pos p = { 0, 0 };
    layout(s, n, &p, 1 << 20);
    return p.col;
}
static void backspaces(struct buf *o, int n) { while (n-- > 0) buf_add(o, "\b", 1); }
static void render_dumb_full(struct editor *e) {
    struct buf *o = &e->out;
    if (!e->fresh) buf_str(o, "\r\n");
    buf_str(o, e->prompt);
    size_t base = 0;
    for (size_t i = 0; i < e->line.n; i++)
        if (e->line.p[i] == '\n') {
            buf_add(o, e->line.p + base, i - base);
            buf_str(o, "\r\n");
            buf_str(o, e->more);
            base = i + 1;
        }
    buf_add(o, e->line.p + base, e->line.n - base);
    buf_set(&e->shown, e->line.p + base, e->line.n - base);
    e->shown_base = base;
    e->shown_cur = e->shown.n;
    e->fresh = false;
}
static void render_dumb(struct editor *e) {
    size_t base = line_start(e, e->line.n);
    if (e->fresh || base != e->shown_base) { render_dumb_full(e); }
    if (e->cur < base) e->cur = base;
    struct buf *o = &e->out;
    const char *now = e->line.p ? e->line.p + base : "";
    size_t n = e->line.n - base, same = 0;
    while (same < n && same < e->shown.n && now[same] == e->shown.p[same]) same++;
    while (same && ((unsigned char)now[same] & 0xc0) == 0x80) same--;
    size_t to = same < e->shown_cur ? same : e->shown_cur;
    backspaces(o, cells(e->shown.p + to, e->shown_cur - to));
    if (to < same) buf_add(o, e->shown.p + to, same - to);       /* forward over unchanged text */
    buf_add(o, now + same, n - same);
    int old_tail = e->shown.n > same ? cells(e->shown.p + same, e->shown.n - same) : 0;
    int new_tail = cells(now + same, n - same);
    for (int i = new_tail; i < old_tail; i++) buf_add(o, " ", 1);
    backspaces(o, old_tail > new_tail ? old_tail - new_tail : 0);
    backspaces(o, cells(now + (e->cur - base), n - (e->cur - base)));
    buf_set(&e->shown, now, n);
    e->shown_cur = e->cur - base;
}
static void render(struct editor *e) {
    if (e->mode == EDIT_VT) render_vt(e);
    else render_dumb(e);
}

/* --- finishing a line ---------------------------------------------------------- */

static void finish(struct editor *e, enum edit_result r, const char *note) {
    if (e->searching) search_end(e, true);
    if (e->menu) menu_close(e);
    e->cur = e->line.n;
    e->accepting = true;
    /* Once a line is done its prompt is drawn the short way, if there is one:
     * the scrollback keeps the commands, not every full prompt. */
    if (e->mode == EDIT_VT && e->transient) {
        free(e->prompt); e->prompt = strdup(e->transient);
        free(e->right); e->right = 0;
    }
    render(e);
    if (note) buf_str(&e->out, note);
    if (e->mode == EDIT_VT) buf_str(&e->out, "\033[?2004l\033[?2048l");
    buf_str(&e->out, "\r\n");
    buf_set(&e->accepted, buf_text(&e->line), e->line.n);
    e->result = r;
}

/* --- keys -> edits ---------------------------------------------------------------- */

static void insert_codepoint(struct editor *e, uint32_t cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    insert(e, b, n);
}
static void case_word(struct editor *e, int how) {
    size_t a = e->cur, b = word_right(e, a);
    bool first = true;
    for (size_t i = a; i < b; i++) {
        char c = e->line.p[i];
        if (!is_word_byte((unsigned char)c)) continue;
        bool up = how == 1 || (how == 2 && first);
        if (up && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!up && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        e->line.p[i] = c;
        first = false;
    }
    e->cur = b;
}
static void yank_last_arg(struct editor *e) {
    if (!hist_count()) return;
    const char *h = hist_get(hist_count() - 1);
    size_t end = strlen(h);
    while (end && is_space(h[end - 1])) end--;
    size_t start = end;
    while (start && !is_space(h[start - 1])) start--;
    insert(e, h + start, end - start);
}

static void accept_or_continue(struct editor *e) {
    if (e->host.incomplete && e->host.incomplete(e->host.ctx, buf_text(&e->line))) {
        snapshot(e);
        e->cur = e->line.n;
        insert(e, "\n", 1);
        return;
    }
    finish(e, EDIT_LINE, 0);
}

/* The search line takes printable keys, Backspace and its own repeats; any
 * other key ends the search with the match kept and then does what it does. */
static bool search_key(struct editor *e, struct key k) {
    if (k.kind == K_TEXT && !(k.mods & M_ALT)) {
        char b[4];
        size_t n = 0;
        uint32_t cp = k.ch;
        if (cp < 0x80) b[n++] = (char)cp;
        else if (cp < 0x800) { b[n++] = (char)(0xc0 | cp >> 6); b[n++] = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { b[n++] = (char)(0xe0 | cp >> 12); b[n++] = (char)(0x80 | (cp >> 6 & 63)); b[n++] = (char)(0x80 | (cp & 63)); }
        else { b[n++] = (char)(0xf0 | cp >> 18); b[n++] = (char)(0x80 | (cp >> 12 & 63)); b[n++] = (char)(0x80 | (cp >> 6 & 63)); b[n++] = (char)(0x80 | (cp & 63)); }
        buf_add(&e->query, b, n);
        size_t from = e->smatch ? e->smatch - 1 : (hist_count() ? hist_count() - 1 : 0);
        if (hist_count()) search_from(e, from, -1);
        return true;
    }
    if (k.kind == K_BACKSPACE) {
        size_t n = e->query.n;
        if (n) {
            do { n--; } while (n && ((unsigned char)e->query.p[n] & 0xc0) == 0x80);
            buf_set(&e->query, e->query.p, n);
        }
        e->smatch = 0;
        if (e->query.n && hist_count()) search_from(e, hist_count() - 1, -1);
        else { e->sfail = false; if (e->sorig) set_line(e, e->sorig, e->sorig_cur); }
        return true;
    }
    if (k.kind == K_CTRL && k.ch == 'r') {
        if (e->smatch > 1 && e->query.n) search_from(e, e->smatch - 2, -1);
        return true;
    }
    if (k.kind == K_CTRL && k.ch == 's') {
        if (e->smatch && e->smatch < hist_count() && e->query.n) search_from(e, e->smatch, +1);
        return true;
    }
    if (k.kind == K_ESCAPE || (k.kind == K_CTRL && k.ch == 'g')) { search_end(e, false); return true; }
    search_end(e, true);
    return false;
}

/* The menu takes Tab, Shift+Tab and the arrows; Enter takes the choice
 * without running the line; Escape puts back what was typed. */
static bool menu_key(struct editor *e, struct key k) {
    struct grid g = menu_grid(e);
    switch (k.kind) {
    case K_TAB: menu_choose(e, e->sel < 0 ? 0 : e->sel + 1); return true;
    case K_BACKTAB: menu_choose(e, e->sel < 0 ? -1 : e->sel - 1); return true;
    case K_DOWN: if (e->sel >= 0) { menu_choose(e, e->sel + 1); return true; } break;
    case K_UP: if (e->sel >= 0) { menu_choose(e, e->sel - 1); return true; } break;
    case K_RIGHT: if (e->sel >= 0) { menu_choose(e, e->sel + g.nrows < (int)e->m.n ? e->sel + g.nrows : e->sel % g.nrows); return true; } break;
    case K_LEFT: if (e->sel >= 0) { menu_choose(e, e->sel - g.nrows >= 0 ? e->sel - g.nrows : e->sel); return true; } break;
    case K_ENTER: if (e->sel >= 0) { bool dir = e->m.items[e->sel].dir; menu_close(e); if (!dir) insert(e, " ", 1); return true; } break;
    case K_ESCAPE:
        if (e->menu_word) replace(e, e->menu_start, e->menu_len, e->menu_word);
        menu_close(e);
        return true;
    default: break;
    }
    if (k.kind == K_CTRL && k.ch == 'g') {
        if (e->menu_word) replace(e, e->menu_start, e->menu_len, e->menu_word);
        menu_close(e);
        return true;
    }
    menu_close(e);
    return false;
}

/* The host's prompt for the width now (edit_host.reprompt). */
static void reprompt(struct editor *e) {
    if (!e->host.reprompt) return;
    struct edit_prompt p = { e->mark, e->prompt, e->more, e->after, e->home, e->right, e->transient };
    e->host.reprompt(e->host.ctx, e->cols, &p);
    char *text = dup_or_null(p.text), *right = dup_or_null(p.right), *transient = dup_or_null(p.transient);
    free(e->prompt); free(e->right); free(e->transient);
    e->prompt = text; e->right = right; e->transient = transient;
}

static void handle(struct editor *e, struct key k) {
    if (k.kind == K_NONE) return;
    if (k.kind == K_RESIZE) {
        if (k.cols > 0 && k.cols != e->cols) { e->cols = k.cols; reprompt(e); }
        /* The first report answers turning reports on and moved nothing.
         * Every later one follows a resize: the Terminal erased the prompt
         * and the next redraw returns to where it began (home). Without home
         * the cursor is taken to be at that origin already. */
        if (e->awaiting_first_report) e->awaiting_first_report = false;
        else if (!e->home) e->fresh = true;
        return;
    }
    if (e->searching && search_key(e, k)) return;
    if (e->menu && menu_key(e, k)) return;

    enum last_action was = e->last;
    e->last = A_OTHER;
    unsigned alt = k.mods & M_ALT, ctrl = k.mods & M_CTRL;

    if (k.kind == K_TEXT || k.kind == K_PASTE_TEXT) {
        if (alt && k.kind == K_TEXT) {
            switch (k.ch) {
            case 'b': case 'B': e->cur = word_left(e, e->cur); return;
            case 'f': case 'F': e->cur = word_right(e, e->cur); return;
            case 'd': case 'D': snapshot(e); { size_t b = word_right(e, e->cur); e->last = was; kill(e, e->cur, b, false); } return;
            case 'u': case 'U': snapshot(e); case_word(e, 1); return;
            case 'l': case 'L': snapshot(e); case_word(e, 0); return;
            case 'c': case 'C': snapshot(e); case_word(e, 2); return;
            case '.': case '_': snapshot(e); yank_last_arg(e); return;
            case '<': if (hist_count()) history_show(e, 0, (size_t)-1); return;
            case '>': history_show(e, hist_count(), (size_t)-1); return;
            case 'y': case 'Y':
                if (was == A_YANK && e->nkills > 1) {
                    erase(e, e->yank_at, e->yank_at + e->yank_len);
                    e->cur = e->yank_at;
                    e->killtop = (e->killtop + KILLS - 1) % KILLS;
                    while (!e->kills[e->killtop]) e->killtop = (e->killtop + KILLS - 1) % KILLS;
                    insert(e, e->kills[e->killtop], strlen(e->kills[e->killtop]));
                    e->yank_len = strlen(e->kills[e->killtop]);
                    e->last = A_YANK;
                }
                return;
            default: return;
            }
        }
        if (was != A_INSERT || k.ch == ' ' || k.kind == K_PASTE_TEXT) snapshot(e);
        insert_codepoint(e, k.ch);
        e->last = A_INSERT;
        return;
    }
    if (k.kind == K_CTRL) {
        switch (k.ch) {
        case 'a': e->cur = line_start(e, e->cur); return;
        case 'e': e->cur = line_end(e, e->cur); return;
        case 'b': e->cur = prev_char(e, e->cur); return;
        case 'f': e->cur = next_char(e, e->cur); return;
        case 'p': k.kind = K_UP; break;
        case 'n': k.kind = K_DOWN; break;
        case 'd':
            if (!e->line.n) { finish(e, EDIT_EOF, 0); return; }
            snapshot(e);
            erase(e, e->cur, next_char(e, e->cur));
            return;
        case 'h': k.kind = K_BACKSPACE; break;
        case 'k': snapshot(e); e->last = was; kill(e, e->cur, line_end(e, e->cur), false); return;
        case 'u': snapshot(e); e->last = was; { size_t a = line_start(e, e->cur); kill(e, a, e->cur, true); } return;
        case 'w': {
            snapshot(e);
            size_t a = e->cur;
            while (a > 0 && is_space(e->line.p[a - 1])) a--;
            while (a > 0 && !is_space(e->line.p[a - 1])) a--;
            e->last = was;
            kill(e, a, e->cur, true);
            return;
        }
        case 'y':
            if (e->nkills) {
                snapshot(e);
                e->yank_at = e->cur;
                insert(e, e->kills[e->killtop], strlen(e->kills[e->killtop]));
                e->yank_len = e->cur - e->yank_at;
                e->last = A_YANK;
            }
            return;
        case 't': {
            if (e->cur == 0 || e->line.n < 2) return;
            snapshot(e);
            if (e->cur == e->line.n) e->cur = prev_char(e, e->cur);
            size_t a = prev_char(e, e->cur), b = next_char(e, e->cur);
            size_t la = e->cur - a, lb = b - e->cur;
            char tmp[64];
            if (la + lb > sizeof tmp) return;
            memcpy(tmp, e->line.p + e->cur, lb);
            memcpy(tmp + lb, e->line.p + a, la);
            memcpy(e->line.p + a, tmp, la + lb);
            e->cur = b;
            return;
        }
        case '_':                              /* Ctrl+_ and Ctrl+/ (both 0x1f): undo */
            if (e->nundo) {
                e->nundo--;
                set_line(e, e->undo[e->nundo].text, e->undo[e->nundo].cur);
                free(e->undo[e->nundo].text);
            }
            return;
        case 'r': search_begin(e); return;
        case 'l':
            if (e->mode == EDIT_VT) buf_str(&e->out, "\033[H\033[2J");
            e->fresh = true;
            return;
        case 'c': finish(e, EDIT_CANCEL, e->mode == EDIT_VT ? "\033[2m^C\033[0m" : "^C"); return;
        case 'g': return;
        default: return;
        }
    }
    switch (k.kind) {
    case K_LEFT: e->cur = ctrl || alt ? word_left(e, e->cur) : prev_char(e, e->cur); return;
    case K_RIGHT: e->cur = ctrl || alt ? word_right(e, e->cur) : next_char(e, e->cur); return;
    case K_HOME: e->cur = line_start(e, e->cur); return;
    case K_END: e->cur = line_end(e, e->cur); return;
    case K_UP:
        if (line_start(e, e->cur) > 0) {       /* within a multi-line program first */
            int col = column_of(e, e->cur);
            e->cur = at_column(e, line_start(e, line_start(e, e->cur) - 1), col);
        } else history_step(e, -1);
        return;
    case K_DOWN:
        if (line_end(e, e->cur) < e->line.n) {
            int col = column_of(e, e->cur);
            e->cur = at_column(e, line_end(e, e->cur) + 1, col);
        } else history_step(e, +1);
        return;
    case K_PGUP: history_prefix(e, -1); return;
    case K_PGDN: history_prefix(e, +1); return;
    case K_BACKSPACE:
        if (alt || ctrl) {                     /* a word back, by word characters */
            snapshot(e);
            e->last = was;
            kill(e, word_left(e, e->cur), e->cur, true);
            return;
        }
        if (e->cur) { if (was != A_INSERT) snapshot(e); erase(e, prev_char(e, e->cur), e->cur); e->last = A_INSERT; }
        return;
    case K_DELETE:
        if (ctrl || alt) { snapshot(e); e->last = was; kill(e, e->cur, word_right(e, e->cur), false); return; }
        if (e->cur < e->line.n) { snapshot(e); erase(e, e->cur, next_char(e, e->cur)); }
        return;
    case K_ENTER:
        if (alt) { snapshot(e); insert(e, "\n", 1); return; }
        accept_or_continue(e);
        return;
    case K_TAB: complete(e, false); return;
    case K_BACKTAB: complete(e, true); return;
    default: return;
    }
}

/* --- suggestions from history ----------------------------------------------------
 * zsh-autosuggestions 0.7.1, its history strategy and default widgets: every
 * key is a widget of one of its kinds. Most MODIFY the line, and the
 * suggestion follows -- kept while the typing goes along it or the line does
 * not change, else the newest history line that starts with the whole line.
 * Some CLEAR it (the history keys, Enter), some ACCEPT it when the cursor is
 * at the end (forward-char, end-of-line), forward-word takes it a word at a
 * time (PARTIAL), and yank leaves it be. */

enum widget { W_MODIFY, W_CLEAR, W_ACCEPT, W_PARTIAL, W_IGNORE };

static enum widget widget_of(struct key k) {
    unsigned alt = k.mods & M_ALT, ctrl = k.mods & M_CTRL;
    switch (k.kind) {
    case K_RIGHT: return ctrl || alt ? W_PARTIAL : W_ACCEPT;
    case K_END: return W_ACCEPT;
    case K_UP: case K_DOWN: case K_PGUP: case K_PGDN: return W_CLEAR;
    case K_ENTER: return alt ? W_MODIFY : W_CLEAR;
    case K_CTRL:
        switch (k.ch) {
        case 'f': case 'e': return W_ACCEPT;
        case 'p': case 'n': return W_CLEAR;
        case 'y': return W_IGNORE;
        case 'c': return W_IGNORE;             /* send-break: the line is left as it was shown */
        default: return W_MODIFY;
        }
    case K_TEXT:
        if (alt && (k.ch == 'f' || k.ch == 'F')) return W_PARTIAL;
        if (alt && (k.ch == 'y' || k.ch == 'Y')) return W_IGNORE;
        return W_MODIFY;
    default: return W_MODIFY;
    }
}

static void suggest(struct editor *e) {
    buf_set(&e->suggestion, "", 0);
    if (!e->line.n || e->searching || e->menu) return;
    for (size_t i = hist_count(); i-- > 0;) {
        const char *h = hist_get(i);
        if (h && !strncmp(h, e->line.p, e->line.n)) {
            buf_set(&e->suggestion, h + e->line.n, strlen(h) - e->line.n);
            return;
        }
    }
}

static void widget(struct editor *e, struct key k) {
    if (e->mode != EDIT_VT) { handle(e, k); return; }
    switch (widget_of(k)) {
    case W_CLEAR:
        buf_set(&e->suggestion, "", 0);
        handle(e, k);
        break;
    case W_IGNORE:
        handle(e, k);
        break;
    case W_ACCEPT:
        if (e->cur != e->line.n || !suggesting(e)) { handle(e, k); break; }
        snapshot(e);
        insert(e, e->suggestion.p, e->suggestion.n);
        buf_set(&e->suggestion, "", 0);
        handle(e, k);
        e->cur = e->line.n;
        break;
    case W_PARTIAL: {
        size_t was = e->line.n;
        char *orig = strdup(buf_text(&e->line));
        if (!orig) { handle(e, k); break; }
        buf_add(&e->line, e->suggestion.p, e->suggestion.n);
        handle(e, k);
        if (e->cur > was) {
            buf_set(&e->suggestion, e->line.p + e->cur, e->line.n - e->cur);
            e->line.n = e->cur;
            e->line.p[e->line.n] = '\0';
        } else {
            buf_set(&e->line, orig, was);
        }
        free(orig);
        break;
    }
    case W_MODIFY: {
        size_t was = e->line.n;
        bool aside = e->searching || e->menu;    /* zsh runs a whole search, a whole menu, as one widget */
        char *orig = strdup(buf_text(&e->line)), *shown = strdup(buf_text(&e->suggestion));
        buf_set(&e->suggestion, "", 0);
        handle(e, k);
        /* typing along the suggestion, or the line as it was: the rest of it */
        if (!aside && orig && shown && e->line.n >= was && !memcmp(e->line.p, orig, was) &&
            !strncmp(shown, e->line.p + was, e->line.n - was) && !e->searching && !e->menu)
            buf_set(&e->suggestion, shown + (e->line.n - was), strlen(shown) - (e->line.n - was));
        else suggest(e);
        free(orig);
        free(shown);
        break;
    }
    }
}

/* --- bytes -> keys -------------------------------------------------------------- */

static struct key key_of(enum key_kind kind, unsigned mods) { struct key k = { kind, 0, mods, 0, 0 }; return k; }

/* A control byte or DEL as a key. */
static struct key control(unsigned char c, unsigned mods) {
    if (c == '\r' || c == '\n') return key_of(K_ENTER, mods);
    if (c == '\t') return key_of(K_TAB, mods);
    if (c == 0x7f || c == 0x08) return key_of(K_BACKSPACE, mods);   /* DEL, and ^H as readline reads it */
    if (c == 0x1b) return key_of(K_ESCAPE, mods);
    struct key k = key_of(K_CTRL, mods);
    k.ch = c == 0 ? '@' : c <= 26 ? (uint32_t)(c - 1 + 'a') : c == 0x1c ? '\\' : c == 0x1d ? ']' : c == 0x1e ? '^' : '_';
    return k;
}
static int params(const char *s, int n, int *out, int max) {
    int count = 0, v = 0, any = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || s[i] == ';') {
            if (count < max) out[count++] = any ? v : -1;
            v = 0; any = 0;
            continue;
        }
        if (s[i] == ':') { while (i + 1 < n && s[i + 1] != ';') i++; continue; }   /* sub-parameters */
        if (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); if (v > 100000) v = 100000; any = 1; }
    }
    return count;
}
static struct key csi(struct editor *e, char final) {
    int p[6] = { -1, -1, -1, -1, -1, -1 };
    int np = params(e->seq, e->seqn, p, 6);
    unsigned mods = np > 1 && p[1] > 1 ? (unsigned)(p[1] - 1) & 7 : 0;
    switch (final) {
    case 'A': return key_of(K_UP, mods);
    case 'B': return key_of(K_DOWN, mods);
    case 'C': return key_of(K_RIGHT, mods);
    case 'D': return key_of(K_LEFT, mods);
    case 'H': return key_of(K_HOME, mods);
    case 'F': return key_of(K_END, mods);
    case 'Z': return key_of(K_BACKTAB, M_SHIFT);
    case 'P': case 'Q': case 'R': case 'S': return key_of(K_FKEY, mods);
    case 't':
        if (p[0] == 48) { struct key k = key_of(K_RESIZE, 0); k.rows = p[1]; k.cols = p[2]; return k; }
        return key_of(K_NONE, 0);
    case '~':
        switch (p[0]) {
        case 1: case 7: return key_of(K_HOME, mods);
        case 4: case 8: return key_of(K_END, mods);
        case 2: return key_of(K_INSERT, mods);
        case 3: return key_of(K_DELETE, mods);
        case 5: return key_of(K_PGUP, mods);
        case 6: return key_of(K_PGDN, mods);
        case 200: e->paste = true; return key_of(K_NONE, 0);
        case 201: e->paste = false; return key_of(K_NONE, 0);
        default: return key_of(p[0] >= 11 && p[0] <= 34 ? K_FKEY : K_NONE, mods);
        }
    default: return key_of(K_NONE, 0);
    }
}
/* One byte in; true when it completed a key. */
static bool decode(struct editor *e, unsigned char c, struct key *k) {
    if (e->mode == EDIT_DUMB) {
        /* The console's private bytes (the kernel keyboard's EK_*): they
         * overlap Ctrl+letters, which the console cannot tell apart. */
        switch (c) {
        case 0x11: *k = key_of(K_LEFT, 0); return true;
        case 0x12: *k = key_of(K_RIGHT, 0); return true;
        case 0x13: *k = key_of(K_UP, 0); return true;
        case 0x14: *k = key_of(K_DOWN, 0); return true;
        case 0x02: *k = key_of(K_HOME, 0); return true;
        case 0x05: *k = key_of(K_END, 0); return true;
        case 0x7f: *k = key_of(K_DELETE, 0); return true;
        case 0x0e: *k = key_of(K_PGUP, 0); return true;
        case 0x0f: *k = key_of(K_PGDN, 0); return true;
        default: break;
        }
    }
    if (e->utfneed) {
        if ((c & 0xc0) != 0x80) { e->utfneed = 0; e->utfn = 0; }   /* broken: restart with this byte */
        else {
            e->utf[e->utfn++] = (char)c;
            if (e->utfn < e->utfneed) return false;
            uint32_t cp;
            utf8_decode(e->utf, e->utfn, &cp);
            e->utfneed = 0;
            *k = key_of(e->paste ? K_PASTE_TEXT : K_TEXT, e->dstate == 1 ? M_ALT : 0);
            k->ch = cp;
            e->dstate = 0;
            return true;
        }
    }
    switch (e->dstate) {
    case 1:                                     /* after ESC */
        if (e->mode == EDIT_VT && c == '[') { e->dstate = 2; e->seqn = 0; return false; }
        if (e->mode == EDIT_VT && c == 'O') { e->dstate = 3; return false; }
        if (c == 0x1b) { *k = key_of(K_ESCAPE, 0); return true; }   /* the first was the key */
        if (c >= 0xc0) { e->utf[0] = (char)c; e->utfn = 1; e->utfneed = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2; return false; }
        e->dstate = 0;
        if (c < 0x20 || c == 0x7f) { *k = control(c, M_ALT); return true; }
        *k = key_of(K_TEXT, M_ALT);
        k->ch = c;
        return true;
    case 2:                                     /* CSI parameters, then the final byte */
        if (c >= 0x40 && c <= 0x7e) { e->dstate = 0; *k = csi(e, (char)c); return true; }
        if (e->seqn < (int)sizeof e->seq) e->seq[e->seqn++] = (char)c;
        return false;
    case 3:                                     /* SS3 x */
        e->dstate = 0;
        switch (c) {
        case 'A': *k = key_of(K_UP, 0); return true;
        case 'B': *k = key_of(K_DOWN, 0); return true;
        case 'C': *k = key_of(K_RIGHT, 0); return true;
        case 'D': *k = key_of(K_LEFT, 0); return true;
        case 'H': *k = key_of(K_HOME, 0); return true;
        case 'F': *k = key_of(K_END, 0); return true;
        case 'M': *k = key_of(K_ENTER, 0); return true;
        default: *k = key_of(K_NONE, 0); return true;
        }
    default: break;
    }
    if (c == 0x1b && e->mode == EDIT_VT) { e->dstate = 1; return false; }
    if (c >= 0xc0) { e->utf[0] = (char)c; e->utfn = 1; e->utfneed = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2; return false; }
    if (c >= 0x80) { *k = key_of(K_NONE, 0); return true; }   /* a stray continuation byte */
    if (e->paste) {
        /* Pasted text is text: a newline is a newline in the program, not
         * Enter, and a tab is a space rather than a completion. CR LF, CR
         * and LF are each one newline. */
        bool after_cr = e->paste_cr;
        e->paste_cr = c == '\r';
        if (c == '\n' && after_cr) { *k = key_of(K_NONE, 0); return true; }
        *k = key_of(K_PASTE_TEXT, 0);
        k->ch = c == '\r' || c == '\n' ? '\n' : c == '\t' ? ' ' : c;
        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') k->kind = K_NONE;
        return true;
    }
    if (c < 0x20 || c == 0x7f) { *k = control(c, 0); return true; }
    *k = key_of(K_TEXT, 0);
    k->ch = c;
    return true;
}

/* --- the interface ---------------------------------------------------------------- */

struct editor *edit_new(const struct edit_host *host) {
    struct editor *e = calloc(1, sizeof *e);
    if (!e) return 0;
    e->host = *host;
    e->cols = 80;
    return e;
}

static void prompt_free(struct editor *e) {
    free(e->mark); free(e->prompt); free(e->more); free(e->after); free(e->home);
    free(e->right); free(e->transient);
    e->mark = e->prompt = e->more = e->after = e->home = e->right = e->transient = 0;
}

void edit_free(struct editor *e) {
    if (!e) return;
    prompt_free(e);
    free(e->line.p); free(e->out.p); free(e->accepted.p); free(e->shown.p);
    free(e->prefix.p); free(e->query.p); free(e->suggestion.p);
    free(e->saved); free(e->sorig); free(e->menu_word);
    for (int i = 0; i < KILLS; i++) free(e->kills[i]);
    for (int i = 0; i < e->nundo; i++) free(e->undo[i].text);
    matches_clear(&e->m);
    free(e);
}


static void flush(struct editor *e) {
    if (e->out.n && e->host.write) e->host.write(e->host.ctx, e->out.p, e->out.n);
    e->out.n = 0;
}

void edit_begin(struct editor *e, const struct edit_prompt *prompt, enum edit_mode mode, int cols) {
    prompt_free(e);
    if (prompt) {
        e->mark = dup_or_null(prompt->mark);
        e->prompt = dup_or_null(prompt->text);
        e->more = dup_or_null(prompt->more);
        e->after = dup_or_null(prompt->after);
        e->home = dup_or_null(prompt->home);
        e->right = dup_or_null(prompt->right);
        e->transient = dup_or_null(prompt->transient);
    }
    e->mode = mode;
    if (cols > 0) e->cols = cols;
    buf_set(&e->line, "", 0);
    e->cur = 0;
    e->result = EDIT_MORE;
    e->dstate = 0; e->seqn = 0; e->utfn = e->utfneed = 0; e->paste = false;
    e->fresh = true;
    e->cursor_row = 0;
    buf_set(&e->shown, "", 0);
    e->shown_cur = e->shown_base = 0;
    e->hpos = hist_count();
    free(e->saved); e->saved = 0;
    e->searching = false;
    buf_set(&e->suggestion, "", 0);
    e->accepting = false;
    menu_close(e);
    for (int i = 0; i < e->nundo; i++) free(e->undo[i].text);
    e->nundo = 0;
    e->last = A_OTHER;
    if (mode == EDIT_VT) {
        buf_str(&e->out, "\033[?2004h\033[?2048h");
        e->awaiting_first_report = true;
    }
    render(e);
    flush(e);
}

enum edit_result edit_feed(struct editor *e, const char *bytes, size_t n, size_t *used) {
    size_t i = 0;
    while (i < n && e->result == EDIT_MORE) {
        struct key k;
        if (decode(e, (unsigned char)bytes[i++], &k)) widget(e, k);
    }
    if (e->result == EDIT_MORE && !edit_pending_escape(e)) render(e);
    flush(e);
    if (used) *used = i;
    return e->result;
}

bool edit_pending_escape(const struct editor *e) { return e->dstate != 0; }

enum edit_result edit_escape_timeout(struct editor *e) {
    if (e->result != EDIT_MORE) return e->result;
    bool lone = e->dstate == 1;
    e->dstate = 0;
    if (lone) widget(e, key_of(K_ESCAPE, 0));
    if (e->result == EDIT_MORE) render(e);
    flush(e);
    return e->result;
}

void edit_reprompt(struct editor *e) {
    if (e->result != EDIT_MORE) return;
    reprompt(e);
    render(e);
    flush(e);
}

void edit_resize(struct editor *e, int cols, bool cleared) {
    if (cols > 0 && cols != e->cols) { e->cols = cols; reprompt(e); }
    if (cleared) e->fresh = true;
    if (e->result == EDIT_MORE) { render(e); flush(e); }
}

const char *edit_line(const struct editor *e) { return buf_text(&e->accepted); }

int edit_cols(const struct editor *e) { return e->cols; }
