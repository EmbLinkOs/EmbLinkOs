/* ==========================================================================
 * wizard.c -- `prompt configure`: Powerlevel10k's configuration wizard,
 * internal/wizard.zsh of p10k 1.20 function for function, by its names
 * (ask_diamond, print_prompt, flowing, render_screen, generate_config). See
 * wizard.h.
 *
 * Text is kept as p10k keeps it -- zsh prompt strings (%B, %2F, %K{236})
 * with escapes () -- and turned into the terminal's bytes where p10k's
 * `print -P` turns it (expand()). A page is widgets with priorities, as
 * p10k's add_widget makes them; render_screen draws the first pass whose lines
 * fit the window. Glyphs in this file are UTF-8 escapes, so every compiler
 * the shell is built with reads it the same.
 * ========================================================================== */
#include "prompt/wizard.h"
#include "prompt/p10k_int.h"
#include "edit/wcwidth.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A space inside one of flowing's words, where p10k quotes them:
 * "%B(y)" NB " Yes." is one word, never broken. */
#define NB "\001"

#define H   "\xe2\x94\x80"      /* ─ */
#define TL  "\xe2\x95\xad"      /* ╭ */
#define BL  "\xe2\x95\xb0"      /* ╰ */
#define TR  "\xe2\x95\xae"      /* ╮ */
#define BR  "\xe2\x95\xaf"      /* ╯ */
#define ML  "\xe2\x94\x9c"      /* ├ */
#define MR  "\xe2\x94\xa4"      /* ┤ */

#define LEFT_CIRCLE    "\\uE0B6"
#define RIGHT_CIRCLE   "\\uE0B4"
#define LEFT_ARC       "\\uE0B7"
#define RIGHT_ARC      "\\uE0B5"
#define LEFT_TRIANGLE  "\\uE0B2"
#define RIGHT_TRIANGLE "\\uE0B0"
#define LEFT_ANGLE     "\\uE0B3"
#define RIGHT_ANGLE    "\\uE0B1"
#define FADE_IN        "\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93"     /* ░▒▓ */
#define FADE_OUT       "\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91"     /* ▓▒░ */
#define VERTICAL_BAR   "\\u2502"
#define CURSOR         "%1{\\e[07m \\e[27m%}"
#define TIME_24H       "16:23:42"
#define TIME_12H       "04:23:42 PM"

enum { GREY, RED, YELLOW, BLUE, MAGENTA, CYAN, WHITE };
static const char *const pure_original[7] = { "242", "1", "3", "4", "5", "6", "7" };
static const char *const pure_snazzy[7] = { "242", "#FF5C57", "#F3F99D", "#57C7FF", "#FF6AC1", "#9AEDFE", "#F1F1F0" };
static const int bg_color[4] = { 240, 238, 236, 234 }, sep_color[4] = { 248, 246, 244, 242 },
                 prefix_color[4] = { 250, 248, 246, 244 };

#define MIN_COLUMNS 47           /* __p9k_wizard_columns */
#define MIN_LINES   14           /* __p9k_wizard_lines */
#define MAX_COLUMNS 88           /* get_columns() */
#define PROMPT_INDENT 2

/* --- the state ------------------------------------------------------------------ */

/* What a sample prompt is drawn from: the answers so far, and print_prompt's
 * own variables. */
struct look {
    struct wizard_answers a;
    char prompt_char[8];
    char buffer[96];           /* typed after the prompt ("": the cursor) */
    bool disable_rprompt;
};

enum { W_MARK, W_PRINT, W_LINES, W_FLOW, W_PROMPT, W_GREETING, W_FRAME_MARKER, W_TAIL_MARKER, W_INDENTED };
struct widget {
    int prio, kind;
    char *text;                /* W_PRINT, W_LINES (\n between), W_FLOW (words), W_INDENTED */
    bool centered;             /* W_FLOW: -c */
    int indent;                /* W_FLOW: -i; W_INDENTED: the width it is centred by */
    struct look *look;         /* W_PROMPT */
};
#define WIDGETS 96

enum { R_OK, R_RESTART, R_QUIT };

struct wiz {
    const struct wizard_io *io;
    const struct wizard_env *env;
    struct look l;
    char down_triangle[16], up_triangle[16], slanted_bar[16];
    int cap_diamond, cap_lock, cap_arrow, cap_quotes;
    const char *color_name[4];
    bool greeting_printed;
    struct widget w[WIDGETS];
    int n, max_priority, prompt_idx;
    int cols, rows;            /* wizard_columns, wizard_lines */
    int pending[32], npending; /* typed while no page could be shown */
    struct sb out;
};

static bool is(const struct look *l, const char *style) { return !strcmp(l->a.style, style); }
static bool lean_any(const struct look *l) { return !strncmp(l->a.style, "lean", 4); }
static bool mode_is(const struct wiz *w, const char *m) { return !strcmp(w->l.a.mode, m); }
static bool nerdfont(const struct wiz *w) { return !strncmp(w->l.a.mode, "nerdfont-", 9); }
static void set(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); }
#define SET(dst, src) set(dst, sizeof dst, src)

static void option(struct wiz *w, const char *opt) {
    char *o = w->l.a.options;
    size_t n = strlen(o);
    snprintf(o + n, sizeof w->l.a.options - n, "%s%s", n ? "\037" : "", opt);
}

/* --- the terminal ----------------------------------------------------------------- */

static void put(struct wiz *w, const char *s) { w->io->write(w->io->ctx, s, strlen(s)); }

/* Text for the terminal: a line ends \r\n, whatever the line discipline does. */
static void put_text(struct wiz *w, const char *s) {
    struct sb t = { 0 };
    for (; *s; s++) {
        if (*s == '\n') sb_chr(&t, '\r');
        sb_chr(&t, *s);
    }
    w->io->write(w->io->ctx, sb_text(&t), t.n);
    sb_free(&t);
}

static void size_now(struct wiz *w, int *cols, int *rows) {
    int c = 80, r = 24;
    w->io->size(w->io->ctx, &c, &r);
    *cols = c;
    *rows = r;
}

/* What a page is written into: its bytes, and the pen the terminal has at
 * their end -- print -P goes on from where the last one left it. */
struct out { struct sb s; struct zpen pen; };

/* `print -P`: the escapes, then the prompt escapes. */
static const struct p10k_state no_state;
static void expand(const char *z, struct out *o) {
    struct sb d = { 0 };
    p9k_decode_escapes(z, &d);
    p9k_zexpand_on(sb_text(&d), &no_state, &o->s, &o->pen);
    sb_free(&d);
}
static void println(struct out *o, const char *z) { expand(z, o); sb_chr(&o->s, '\n'); }

/* prompt_length of ${(g::)z}: the cells it takes once expanded. */
static int width(const char *z) {
    struct sb d = { 0 };
    p9k_decode_escapes(z, &d);
    int n = p10k_width(sb_text(&d), &no_state);
    sb_free(&d);
    return n;
}

static void spaces(struct sb *out, int n) { for (int i = 0; i < n; i++) sb_chr(out, ' '); }

/* flowing [-c] [-i indent] words...: the words in lines no wider than the
 * page, each centred with -c. */
static void flowing(struct out *o, int cols, bool centered, int indent, const char *text) {
    struct sb line = { 0 }, probe = { 0 }, lines = { 0 };
    int nlines = 0;
    for (const char *p = text; *p;) {
        size_t len = strcspn(p, " ");
        struct sb word = { 0 };
        for (size_t i = 0; i < len; i++) sb_chr(&word, p[i] == '\001' ? ' ' : p[i]);
        p += len;
        if (*p == ' ') p++;
        sb_reset(&probe);
        sb_str(&probe, sb_text(&line));
        sb_chr(&probe, ' ');
        sb_str(&probe, sb_text(&word));
        if (width(sb_text(&probe)) > cols) {
            if (line.n) { sb_str(&lines, sb_text(&line)); sb_chr(&lines, '\n'); nlines++; }
            sb_reset(&line);
        }
        if (line.n) sb_chr(&line, ' ');
        else if (nlines) spaces(&line, indent);
        sb_str(&line, sb_text(&word));
        sb_free(&word);
    }
    if (line.n) { sb_str(&lines, sb_text(&line)); sb_chr(&lines, '\n'); }
    for (const char *p = sb_text(&lines); *p;) {
        size_t len = strcspn(p, "\n");
        struct sb one = { 0 };
        sb_add(&one, p, len);
        int n = width(sb_text(&one));
        if (centered && n < cols) spaces(&o->s, (cols - n) / 2);
        println(o, sb_text(&one));
        sb_free(&one);
        p += len + (p[len] == '\n');
    }
    sb_free(&line);
    sb_free(&probe);
    sb_free(&lines);
}

/* href: a link the terminal can open, and its text. */
static void href(struct sb *out, const char *url) {
    struct sb u = { 0 };
    for (const char *p = url; *p; p++) {
        if (*p == '%') sb_str(&u, "%%");
        else if (*p == '\\') sb_str(&u, "\\\\");
        else sb_chr(&u, *p);
    }
    sb_fmt(out, "%%{\\e]8;;%s\\a%%}%s%%{\\e]8;;\\a%%}", sb_text(&u), sb_text(&u));
    sb_free(&u);
}

/* --- the sample prompts ---------------------------------------------------------- */

/* ${style}_left and ${style}_right, evaluated: four parts each -- the first
 * line's frame and content, the last line's. */
static void templates(const struct look *l, struct sb left[4], struct sb right[4]) {
    const struct wizard_answers *a = &l->a;
    const char *ei[5], *pf[3];
    for (int i = 0; i < 5; i++) ei[i] = a->extra_icons[i];
    for (int i = 0; i < 3; i++) pf[i] = a->prefixes[i];
    int fc = a->frame_color[a->color - 1];
    const char *buf = l->buffer[0] ? l->buffer : CURSOR;
    if (is(l, "lean") || is(l, "lean_8colors")) {
        bool eight = is(l, "lean_8colors");
        sb_fmt(&left[0], "%%%dF" TL H " ", fc);
        if (ei[0][0]) sb_fmt(&left[1], "%%f%s ", ei[0]);
        if (eight) sb_fmt(&left[1], "%%4F%s%%4F~/src%%f %s%%2F%smaster%%f ", ei[1], pf[0], ei[2]);
        else sb_fmt(&left[1], "%%31F%s%%B%%39F~%%b%%31F/%%B%%39Fsrc%%b%%f %s%%76F%smaster%%f ", ei[1], pf[0], ei[2]);
        sb_fmt(&left[2], "%%%dF" BL H, fc);
        sb_fmt(&left[3], "%%%dF%s%%f %s", eight ? 2 : 76, l->prompt_char, buf);
        sb_fmt(&right[0], " %s%%%dF%s5s%%f", pf[1], eight ? 3 : 101, ei[3]);
        if (a->time[0]) sb_fmt(&right[0], " %s%%%dF%s%s%%f", pf[2], eight ? 6 : 66, ei[4], a->time);
        sb_fmt(&right[1], " %%%dF" H TR "%%f", fc);
        sb_fmt(&right[3], " %%%dF" H BR "%%f", fc);
    } else if (is(l, "classic")) {
        int bg = bg_color[a->color - 1], sep = sep_color[a->color - 1], pc = prefix_color[a->color - 1];
        sb_fmt(&left[0], "%%%dF" TL H, fc);
        sb_fmt(&left[1], "%%F{%d}%s%%K{%d} ", bg, a->left_tail, bg);
        if (ei[0][0]) sb_fmt(&left[1], "%%255F%s %%%dF%s%%f ", ei[0], sep, a->left_subsep);
        sb_fmt(&left[1], "%%31F%s%%B%%39F~%%b%%K{%d}%%31F/%%B%%39Fsrc%%b%%K{%d} ", ei[1], bg, bg);
        sb_fmt(&left[1], "%%%dF%s%%f %%%dF%s%%76F%smaster %%k%%%dF%s%%f", sep, a->left_subsep, pc, pf[0], ei[2], bg,
               a->left_head);
        sb_fmt(&left[2], "%%%dF" BL H, fc);
        sb_fmt(&left[3], "%%f %s", buf);
        sb_fmt(&right[0], "%%%dF%s%%K{%d}%%f %%%dF%s%%101F5s %s", bg, a->right_head, bg, pc, pf[1], ei[3]);
        if (a->time[0])
            sb_fmt(&right[0], "%%%dF%s %%%dF%s%%66F%s %s", sep, a->right_subsep, pc, pf[2], a->time, ei[4]);
        sb_fmt(&right[0], "%%k%%F{%d}%s%%f", bg, a->right_tail);
        sb_fmt(&right[1], "%%%dF" H TR "%%f", fc);
        sb_fmt(&right[3], "%%%dF" H BR "%%f", fc);
    } else if (is(l, "rainbow")) {
        sb_fmt(&left[0], "%%%dF" TL H, fc);
        sb_fmt(&left[1], "%%F{%d}%s", ei[0][0] ? 7 : 4, a->left_tail);
        if (ei[0][0]) sb_fmt(&left[1], "%%K{7}%%232F %s %%K{4}%%7F%s", ei[0], a->left_sep);
        sb_fmt(&left[1], "%%K{4}%%254F %s%%B%%255F~%%b%%K{4}%%254F/%%B%%255Fsrc%%b%%K{4} ", ei[1]);
        sb_fmt(&left[1], "%%K{2}%%4F%s %%0F%s%smaster %%k%%2F%s%%f", a->left_sep, pf[0], ei[2], a->left_head);
        sb_fmt(&left[2], "%%%dF" BL H, fc);
        sb_fmt(&left[3], "%%f %s", buf);
        sb_fmt(&right[0], "%%3F%s%%K{3} %%0F%s5s %s%%3F", a->right_head, pf[1], ei[3]);
        if (a->time[0]) sb_fmt(&right[0], "%%7F%s%%K{7} %%0F%s%s %s%%7F", a->right_sep, pf[2], a->time, ei[4]);
        sb_fmt(&right[0], "%%k%s%%f", a->right_tail);
        sb_fmt(&right[1], "%%%dF" H TR "%%f", fc);
        sb_fmt(&right[3], "%%%dF" H BR "%%f", fc);
    } else {                                           /* pure */
        sb_fmt(&left[1], "%%F{%s}~/src%%f %%F{%s}master%%f ", a->pure_color[BLUE], a->pure_color[GREY]);
        if (!a->pure_use_rprompt) sb_fmt(&left[1], "%%F{%s}5s%%f ", a->pure_color[YELLOW]);
        sb_fmt(&left[3], "%%F{%s}%s%%f %s", a->pure_color[MAGENTA], l->prompt_char, buf);
        if (a->pure_use_rprompt) sb_fmt(&right[0], "%%F{%s}5s%%f%s", a->pure_color[YELLOW], a->time[0] ? " " : "");
        if (a->time[0]) sb_fmt(&right[0], "%%F{%s}%s%%f", a->pure_color[GREY], a->time);
    }
}

/* print_prompt: the sample, indented, its gap filled to the page's width. */
static void print_prompt(const struct wiz *w, const struct look *l, struct out *out) {
    const struct wizard_answers *a = &l->a;
    struct sb L[4] = { { 0 } }, R[4] = { { 0 } }, last = { 0 };
    templates(l, L, R);
    if (l->disable_rprompt) for (int i = 0; i < 4; i++) sb_reset(&R[i]);
    /* zsh's arrays: an unquoted empty element is no element */
    const char *left[4], *right[4];
    int nl = 0, nr = 0;
    if (a->num_lines == 1) {
        if (L[1].n) left[nl++] = sb_text(&L[1]);
        if (L[3].n) left[nl++] = sb_text(&L[3]);
        if (R[0].n) right[nr++] = sb_text(&R[0]);
        if (R[2].n) right[nr++] = sb_text(&R[2]);
    } else {
        for (int i = 0; i < 4; i++) left[nl++] = sb_text(&L[i]);
        for (int i = 0; i < 4; i++) if (!l->disable_rprompt) right[nr++] = sb_text(&R[i]);
        char c[16];
        snprintf(c, sizeof c, "%s", is(l, "pure") ? a->pure_color[MAGENTA] : is(l, "lean_8colors") ? "2" : "76");
        if (!a->left_frame) {
            sb_fmt(&last, "%%F{%s}%s%%f %s", c, l->prompt_char, l->buffer[0] ? l->buffer : CURSOR);
            nl = 0;
            left[nl++] = "";
            if (L[1].n) left[nl++] = sb_text(&L[1]);
            left[nl++] = "";
            left[nl++] = sb_text(&last);
        }
        if (!a->right_frame) {
            const char *first = nr ? right[0] : "";
            nr = 0;
            if (first[0]) right[nr++] = first;
            right[nr++] = "";
            right[nr++] = "";
            right[nr++] = "";
        }
    }
#define AT(arr, n, i) ((i) < (n) ? (arr)[i] : "")
    int li = PROMPT_INDENT, ri = PROMPT_INDENT;
    struct sb t = { 0 };
    sb_str(&t, AT(left, nl, 0));
    sb_str(&t, AT(left, nl, 1));
    sb_str(&t, AT(right, nr, 0));
    sb_str(&t, AT(right, nr, 1));
    int wd = width(sb_text(&t));
    bool room = true;
    while (w->cols - wd <= li + ri) {
        if (ri) ri--;
        else if (li) li--;
        else { println(out, "  [%3Fnot enough horizontal space to display this%f]"); room = false; break; }
    }
    for (int i = 0; room && i + 1 < nl; i += 2) {
        struct sb lt = { 0 }, rt = { 0 }, gap = { 0 }, both = { 0 };
        sb_str(&lt, AT(left, nl, i));
        sb_str(&lt, AT(left, nl, i + 1));
        sb_str(&rt, AT(right, nr, i));
        sb_str(&rt, AT(right, nr, i + 1));
        sb_str(&both, sb_text(&lt));
        sb_str(&both, sb_text(&rt));
        int g = w->cols - li - ri - width(sb_text(&both));
        const char *fill = a->num_lines == 2 && i == 0 ? a->gap_char : " ";
        sb_fmt(&gap, "%%%dF", a->frame_color[a->color - 1]);
        for (int k = 0; k < g; k++) sb_str(&gap, fill);
        sb_str(&gap, "%f");
        spaces(&out->s, li);
        struct sb d = { 0 };
        p9k_decode_escapes(sb_text(&lt), &d);
        expand(sb_text(&d), out);
        expand(sb_text(&gap), out);
        sb_reset(&d);
        p9k_decode_escapes(sb_text(&rt), &d);
        println(out, sb_text(&d));
        sb_free(&d); sb_free(&lt); sb_free(&rt); sb_free(&gap); sb_free(&both);
    }
#undef AT
    sb_free(&t);
    sb_free(&last);
    for (int i = 0; i < 4; i++) { sb_free(&L[i]); sb_free(&R[i]); }
}

/* --- pages ---------------------------------------------------------------------- */

static struct widget *add(struct wiz *w, int prio, int kind, const char *text) {
    if (w->n == WIDGETS) return NULL;
    struct widget *x = &w->w[w->n++];
    memset(x, 0, sizeof *x);
    x->prio = prio;
    x->kind = kind;
    if (text) {
        x->text = malloc(strlen(text) + 1);
        if (x->text) memcpy(x->text, text, strlen(text) + 1);
    }
    if (prio > w->max_priority) w->max_priority = prio;
    return x;
}
static void mark(struct wiz *w, int prio) { add(w, prio, W_MARK, NULL); }
static void print_(struct wiz *w, const char *z) { add(w, 0, W_PRINT, z); }
static void flow(struct wiz *w, bool centered, int indent, const char *words) {
    struct widget *x = add(w, 0, W_FLOW, words);
    if (x) { x->centered = centered; x->indent = indent; }
}
static void flow_c(struct wiz *w, const char *words) { flow(w, true, 0, words); }

static void clear_widgets(struct wiz *w) {
    for (int i = 0; i < w->n; i++) { free(w->w[i].text); free(w->w[i].look); }
    w->n = 0;
    w->max_priority = 0;
    w->prompt_idx = 0;
}

/* add_prompt_n: a sample prompt, and what stands for it when a page is short. */
static void add_prompt_n(struct wiz *w, const struct look *l) {
    struct widget *x = add(w, 0, W_PROMPT, NULL);
    if (x && (x->look = malloc(sizeof *l))) *x->look = *l;
    if (l->a.num_lines == 2)
        add(w, 100 - ++w->prompt_idx, W_PRINT, "  [%3Fnot enough vertical space to display this%f]");
}
static void add_prompt(struct wiz *w, const struct look *l) {
    print_(w, "");
    mark(w, 1);
    add_prompt_n(w, l);
    print_(w, "");
    mark(w, 2);
}
/* The answers so far, to be changed for one sample. */
static struct look now(const struct wiz *w) { return w->l; }

static void greeting(const struct wiz *w, struct out *out) {
    if (w->greeting_printed) return;
    if (w->env->force)
        flowing(out, w->cols, true, 0, "This is %4FPowerlevel10k configuration wizard%f. "
                                       "It will ask you a few questions and configure your prompt.");
    else
        flowing(out, w->cols, true, 0, "This is %4FPowerlevel10k configuration wizard%f. "
                                       "You are seeing it because you haven't defined any "
                                       "Powerlevel10k configuration options. It will ask "
                                       "you a few questions and configure your prompt.");
    println(out, "");
}

static void render(const struct wiz *w, const struct widget *x, struct out *out) {
    char buf[160];
    switch (x->kind) {
    case W_PRINT: println(out, x->text); break;
    case W_LINES:
        for (const char *p = x->text; *p;) {
            size_t len = strcspn(p, "\n");
            snprintf(buf, sizeof buf, "%.*s", (int)len, p);
            println(out, buf);
            p += len + (p[len] == '\n');
        }
        break;
    case W_FLOW: flowing(out, w->cols, x->centered, x->indent, x->text); break;
    case W_PROMPT: print_prompt(w, x->look, out); break;
    case W_GREETING: greeting(w, out); break;
    case W_FRAME_MARKER: case W_TAIL_MARKER: {
        bool frame = x->kind == W_FRAME_MARKER;
        char label[48];
        if (frame) snprintf(label, sizeof label, "(1)  %s.", w->color_name[0]);
        else snprintf(label, sizeof label, "(1)  Flat.");
        int n = w->cols - 7, m = n - (int)strlen(label);
        struct sb s = { 0 };
        spaces(&s, n);
        sb_str(&s, frame ? "frame" : "tail");
        println(out, sb_text(&s));
        sb_reset(&s);
        sb_fmt(&s, "%%B%s%%b", label);
        spaces(&s, m);
        sb_str(&s, frame ? "    |" : "  |");
        println(out, sb_text(&s));
        sb_reset(&s);
        spaces(&s, n);
        sb_str(&s, frame ? "    v" : "  v");
        println(out, sb_text(&s));
        sb_free(&s);
        break;
    }
    case W_INDENTED: {
        struct sb s = { 0 };
        spaces(&s, (w->cols - x->indent) / 2);
        sb_str(&s, x->text);
        println(out, sb_text(&s));
        sb_free(&s);
        break;
    }
    default: break;
    }
}

/* render_screen_pass: a widget is drawn unless one after it marks it as
 * left out at this pass, and one with a priority only from its pass on. */
static void render_pass(const struct wiz *w, int pass, struct out *out) {
    int prev_pass = 0;
    const struct widget *prev = NULL;
    for (int i = 0; i <= w->n; i++) {
        int cur_pass = i < w->n ? w->w[i].prio : 0;
        if (prev && prev_pass <= pass && (cur_pass == 0 || cur_pass > pass)) render(w, prev, out);
        prev_pass = cur_pass;
        prev = i < w->n ? &w->w[i] : NULL;
    }
}

static void clear_screen(struct wiz *w) { put(w, "\033[H\033[2J\033[3J"); }

static void too_small(struct wiz *w, const char *what, const char *how) {
    struct out o = { { 0 }, { "", "", false, false, false } };
    struct sb t = { 0 };
    clear_screen(w);
    sb_fmt(&t, "%%1FNot enough %s space.%%f", what);
    flowing(&o, w->cols, true, 0, sb_text(&t));
    sb_chr(&o.s, '\n');
    sb_reset(&t);
    sb_fmt(&t, "Make terminal window %%B%s%%b or press %%BCtrl-C%%b to abort Powerlevel10k configuration wizard.", how);
    flowing(&o, w->cols, false, 0, sb_text(&t));
    put_text(w, sb_text(&o.s));
    sb_free(&o.s);
    sb_free(&t);
}

/* A key that is not Ctrl+C, the end or q waits for the page. */
static bool hold(struct wiz *w, int k) {
    if (k == 3 || k == WIZARD_EOF) return false;
    if (k >= 0 && w->npending < (int)(sizeof w->pending / sizeof w->pending[0])) w->pending[w->npending++] = k;
    return true;
}

/* Until the window is another size; false: Ctrl+C. */
static bool wait_resize(struct wiz *w) {
    for (;;) {
        int c, r;
        size_now(w, &c, &r);
        if ((c > MAX_COLUMNS ? MAX_COLUMNS : c) != w->cols || r != w->rows) return true;
        if (!hold(w, w->io->key(w->io->ctx, 1000))) return false;
    }
}

/* render_screen: the first pass that fits; false: Ctrl+C while the window
 * was too small for any. */
static bool render_screen(struct wiz *w) {
    put(w, "\033[?25l");
    bool ok = true;
    for (;;) {
        for (;;) {
            int c, r;
            size_now(w, &c, &r);
            w->cols = c > MAX_COLUMNS ? MAX_COLUMNS : c;
            w->rows = r;
            if (w->cols < MIN_COLUMNS) too_small(w, "horizontal", "wider");
            else if (w->rows < MIN_LINES) too_small(w, "vertical", "taller");
            else break;
            if (!wait_resize(w)) { ok = false; goto done; }
        }
        int passes[WIDGETS + 1], np = 0;
        for (int i = 0; i < w->n; i++) {
            int p = w->w[i].prio, k = 0;
            while (k < np && passes[k] < p) k++;
            if (k < np && passes[k] == p) continue;
            memmove(passes + k + 1, passes + k, (size_t)(np - k) * sizeof *passes);
            passes[k] = p;
            np++;
        }
        for (int i = 0; i < np; i++) {
            struct out content = { { 0 }, { "", "", false, false, false } };
            struct sb *c = &content.s;
            render_pass(w, passes[i], &content);
            while (c->n && c->p[c->n - 1] == '\n') c->p[--c->n] = 0;           /* $(...) */
            int lines = 1;
            for (size_t k = 0; k < c->n; k++) lines += c->p[k] == '\n';
            if (lines > w->rows) { sb_free(c); continue; }
            clear_screen(w);
            put_text(w, sb_text(c));
            sb_free(c);
            goto done;
        }
        too_small(w, "vertical", "taller");
        if (!wait_resize(w)) { ok = false; goto done; }
    }
done:
    put(w, "\033[?25h");
    return ok;
}

/* ask: the page with its choices, until one of them (or q) is typed; the
 * choice, or 0 to quit. */
static char ask(struct wiz *w, const char *choices) {
    char choice[40];
    print_(w, "(q)  Quit and do nothing.");
    print_(w, "");
    mark(w, w->max_priority + 1);
    snprintf(choice, sizeof choice, "%%BChoice [%sq]: %%b", choices);
    print_(w, choice);
    int lines = 0, columns = 0;
    for (;;) {
        int c, r;
        size_now(w, &c, &r);
        if (r != lines || (c > MAX_COLUMNS ? MAX_COLUMNS : c) != columns) {
            if (!render_screen(w)) return 0;
            lines = w->rows;
            columns = w->cols;
        }
        int k;
        if (w->npending) {
            k = w->pending[0];
            memmove(w->pending, w->pending + 1, (size_t)--w->npending * sizeof *w->pending);
        } else k = w->io->key(w->io->ctx, 1000);
        if (k == WIZARD_TIMEOUT || k == WIZARD_RESIZED) continue;
        if (k == WIZARD_EOF || k == 3) return 0;
        char ch = (char)tolower(k);
        if (ch == 'q') return 0;
        if (ch && strchr(choices, ch)) {
            clear_widgets(w);
            return ch;
        }
    }
}

/* --- the questions ------------------------------------------------------------------ */

static int yes_no(struct wiz *w, const char *choices, int *cap) {
    char c = ask(w, choices);
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    *cap = c == 'y';
    return R_OK;
}

static void choices_y_n(struct wiz *w, bool restart) {
    mark(w, 3);
    print_(w, "%B(y)  Yes.%b");
    print_(w, "");
    mark(w, 1);
    print_(w, "%B(n)  No.%b");
    print_(w, "");
    mark(w, 2);
    if (restart) print_(w, "(r)  Restart from the beginning.");
}

static void arrows(struct wiz *w, const char *glyphs) {
    char word[96];
    snprintf(word, sizeof word, "--->" NB NB "%s" NB NB "<---", glyphs);
    flow_c(w, word);
}

static void reference(struct wiz *w, const char *url, bool one_word) {
    struct sb s = { 0 };
    sb_str(&s, one_word ? "reference:" NB : "reference: ");
    href(&s, url);
    flow_c(w, sb_text(&s));
    sb_free(&s);
}

static int ask_diamond(struct wiz *w) {
    add(w, 0, W_GREETING, NULL);
    flow_c(w, "%BDoes this look like a%b %2Fdiamond%f %B(rotated" NB "square)?%b");
    reference(w, "https://graphemica.com/%E2%97%86", false);
    print_(w, "");
    arrows(w, "\\uE0B2\\uE0B0");
    print_(w, "");
    choices_y_n(w, false);       /* (r) only where p10k can install a font: iTerm2, Termux */
    int r = yes_no(w, "yn", &w->cap_diamond);
    w->greeting_printed = true;
    return r;
}

static int ask_lock(struct wiz *w, const char *glyph, const char *before) {
    if (before) flow_c(w, before);
    flow_c(w, "%BDoes" NB "this" NB "look" NB "like" NB "a" NB "%b%2Flock%f%B?%b");
    reference(w, "https://fontawesome.com/icons/lock", true);
    print_(w, "");
    arrows(w, glyph);
    print_(w, "");
    choices_y_n(w, true);
    return yes_no(w, "ynr", &w->cap_lock);
}

static int ask_python(struct wiz *w) {
    flow_c(w, "%BDoes this look like a %b%2FPython" NB "logo%f%B?%b");
    reference(w, "https://fontawesome.com/icons/python", false);
    print_(w, "");
    arrows(w, "\\uE63C");
    print_(w, "");
    choices_y_n(w, true);
    return yes_no(w, "ynr", &w->l.a.cap_python);
}

static int ask_quotes(struct wiz *w) {
    flow_c(w, "%BDoes this look like%b %2F><%f %Bbut taller and fatter?%b");
    print_(w, "");
    arrows(w, "\\u276F\\u276E");
    print_(w, "");
    choices_y_n(w, true);
    return yes_no(w, "ynr", &w->cap_quotes);
}

static int ask_arrow(struct wiz *w, const char *glyph, const char *before) {
    if (width(glyph) != 1) { w->cap_arrow = 0; return R_OK; }   /* zsh without unicode 9 */
    if (before) flow_c(w, before);
    flow_c(w, "%BDoes this look like an%b %2Fupwards" NB "arrow%f%B?%b");
    reference(w, "https://graphemica.com/%F0%9F%A0%89", false);
    print_(w, "");
    arrows(w, glyph);
    print_(w, "");
    choices_y_n(w, true);
    return yes_no(w, "ynr", &w->cap_arrow);
}

static void indented(struct wiz *w, int max_width, const char *text) {
    struct widget *x = add(w, 0, W_INDENTED, text);
    if (x) x->indent = max_width;
}

static int ask_width(struct wiz *w) {
    flow_c(w, "%BWhat digit is the%b %2Fdownwards" NB "arrow%f %Bpointing at?%b");
    print_(w, "");
    indented(w, 11, "%3F\\UF0734%f %3F\\UF0734%f %3F\\UF0734%f %2F\\UF072E%f");
    indented(w, 11, "     111222");
    print_(w, "");
    mark(w, 3);
    print_(w, "%B(1)  It is pointing at '1'.%b");
    print_(w, "");
    mark(w, 1);
    print_(w, "%B(2)  It is pointing at '2'.%b");
    print_(w, "");
    mark(w, 1);
    print_(w, "%B(3)  Something else.%b");
    print_(w, "");
    mark(w, 2);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "123r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    w->cap_arrow = c == '1';
    return R_OK;
}

/* icons[name] after _p9k_init_icons with the mode and padding so far. */
static void icon(const struct wiz *w, const char *name, struct sb *out) {
    struct p10k *cfg = p10k_new();
    sb_reset(out);
    if (!cfg) return;
    p10k_set(cfg, "MODE", w->l.a.mode);
    p10k_set(cfg, "ICON_PADDING", w->l.a.icon_padding);
    struct sb v = { 0 };
    p9k_icon(cfg, "", name, "", &v);
    sb_str(out, sb_text(&v));
    sb_free(&v);
    p10k_free(cfg);
}
static void icon_nospace(const struct wiz *w, const char *name, struct sb *out) {
    struct sb v = { 0 };
    icon(w, name, &v);
    sb_reset(out);
    for (const char *p = sb_text(&v); *p; p++) if (*p != ' ') sb_chr(out, *p);
    sb_free(&v);
}

static int ask_icon_padding(struct wiz *w) {
    if (mode_is(w, "powerline") || mode_is(w, "compatible") || mode_is(w, "ascii")) {
        SET(w->l.a.icon_padding, "none");
        return R_OK;
    }
    static const char *const names[] = { "VCS_GIT_ICON", "VCS_GIT_GITHUB_ICON", "TIME_ICON", "RUBY_ICON", "HOME_ICON",
                                         "HOME_SUB_ICON", "FOLDER_ICON", "RAM_ICON" };
    static const int colors[] = { 1, 2, 3, 4, 5, 6, 1, 2 };
    struct sb text = { 0 }, ic = { 0 };
    sb_str(&text, "--->" NB NB "X");
    for (int i = 0; i < 8; i++) {
        icon_nospace(w, names[i], &ic);
        sb_fmt(&text, "%%%dF%s%%fX", colors[i], sb_text(&ic));
    }
    sb_str(&text, NB NB "<---");
    flow_c(w, "%BDo all these icons %b%2Ffit" NB "between" NB "the" NB "crosses%f%B?%b");
    print_(w, "");
    flow_c(w, sb_text(&text));
    sb_free(&text);
    sb_free(&ic);
    print_(w, "");
    mark(w, 3);
    flow(w, false, 5, "%B(y)" NB NB "Yes. Icons are very close to the crosses but there is %b%2Fno" NB "overlap%f%B.%b");
    print_(w, "");
    mark(w, 1);
    flow(w, false, 5, "%B(n)" NB NB "No. Some icons %b%2Foverlap%f%B neighbouring crosses.%b");
    print_(w, "");
    mark(w, 2);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "ynr");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    if (c == 'y') {
        SET(w->l.a.icon_padding, "none");
        option(w, "small icons");
    } else {
        SET(w->l.a.icon_padding, "moderate");
        option(w, "large icons");
        strncat(w->up_triangle, " ", sizeof w->up_triangle - strlen(w->up_triangle) - 1);
        strncat(w->down_triangle, " ", sizeof w->down_triangle - strlen(w->down_triangle) - 1);
        SET(w->slanted_bar, "\\uE0BD ");
    }
    return R_OK;
}

static void title(struct wiz *w, const char *words) {
    struct sb s = { 0 };
    sb_str(&s, "%B");
    for (const char *p = words; *p; p++) {
        if (*p == ' ') sb_str(&s, NB);
        else sb_chr(&s, *p);
    }
    sb_str(&s, "%b");
    flow_c(w, sb_text(&s));
    sb_free(&s);
}

/* A choice's label, then its sample prompt. */
static void labelled(struct wiz *w, const char *label, const struct look *l) {
    char z[96];
    snprintf(z, sizeof z, "%%B%s%%b", label);
    print_(w, z);
    add_prompt(w, l);
}

static int ask_style(struct wiz *w) {
    struct wizard_answers *a = &w->l.a;
    static const int eight[4] = { 0, 7, 2, 4 };
    static const char *const eight_names[4] = { "Black", "White", "Green", "Blue" };
    if (!w->env->colors256) {
        SET(a->style, "lean_8colors");
        a->left_frame = a->right_frame = 0;
        memcpy(a->frame_color, eight, sizeof eight);
        memcpy(w->color_name, eight_names, sizeof eight_names);
        option(w, "lean_8colors");
        return R_OK;
    }
    title(w, "Prompt Style");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    SET(l.a.style, "lean");
    l.a.left_frame = l.a.right_frame = 0;
    labelled(w, "(1)  Lean.", &l);
    l = now(w);
    SET(l.a.style, "classic");
    labelled(w, "(2)  Classic.", &l);
    SET(l.a.style, "rainbow");
    labelled(w, "(3)  Rainbow.", &l);
    bool pure = !mode_is(w, "ascii");
    if (pure) {
        SET(l.a.style, "pure");
        labelled(w, "(4)  Pure.", &l);
    }
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, pure ? "1234r" : "123r");
    switch (c) {
    case 0: return R_QUIT;
    case 'r': return R_RESTART;
    case '1': SET(a->style, "lean"); a->left_frame = a->right_frame = 0; option(w, "lean"); break;
    case '2': SET(a->style, "classic"); option(w, "classic"); break;
    case '3': SET(a->style, "rainbow"); option(w, "rainbow"); break;
    default: SET(a->style, "pure"); a->empty_line = 1; option(w, "pure"); break;
    }
    return R_OK;
}

static void ascii_look(struct look *l) {
    struct wizard_answers *a = &l->a;
    a->left_sep[0] = a->right_sep[0] = a->left_head[0] = a->right_head[0] = 0;
    SET(a->left_subsep, "|");
    SET(a->right_subsep, "|");
    SET(l->prompt_char, ">");
    a->left_frame = a->right_frame = 0;
}

static int ask_charset(struct wiz *w) {
    if (!(lean_any(&w->l) || is(&w->l, "classic") || is(&w->l, "rainbow")) || mode_is(w, "ascii")) return R_OK;
    title(w, "Character Set");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    labelled(w, "(1)  Unicode.", &l);
    ascii_look(&l);
    labelled(w, "(2)  ASCII.", &l);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    if (c == '1') { option(w, "unicode"); return R_OK; }
    option(w, "ascii");
    ascii_look(&w->l);
    SET(w->l.a.mode, "ascii");
    SET(w->l.a.icon_padding, "none");
    w->cap_diamond = w->l.a.cap_python = w->cap_arrow = w->cap_lock = w->cap_quotes = 0;
    return R_OK;
}

static int ask_color_scheme(struct wiz *w) {
    struct wizard_answers *a = &w->l.a;
    if (!w->env->colors256) return R_OK;
    if (is(&w->l, "lean")) {
        title(w, "Prompt Colors");
        print_(w, "");
        mark(w, 1);
        struct look l = now(w);
        labelled(w, "(1)  256 colors.", &l);
        SET(l.a.style, "lean_8colors");
        labelled(w, "(2)  8 colors.", &l);
        print_(w, "(r)  Restart from the beginning.");
        char c = ask(w, "12r");
        if (!c) return R_QUIT;
        if (c == 'r') return R_RESTART;
        if (c == '2') {
            static const int eight[4] = { 0, 7, 2, 4 };
            static const char *const eight_names[4] = { "Black", "White", "Green", "Blue" };
            SET(a->style, "lean_8colors");
            memcpy(a->frame_color, eight, sizeof eight);
            memcpy(w->color_name, eight_names, sizeof eight_names);
        }
        /* options=(${options:#lean} $style) */
        char kept[sizeof a->options] = "";
        for (const char *p = a->options; *p;) {
            size_t len = strcspn(p, "\037");
            if (!(len == 4 && !strncmp(p, "lean", 4))) {
                size_t k = strlen(kept);
                snprintf(kept + k, sizeof kept - k, "%s%.*s", k ? "\037" : "", (int)len, p);
            }
            p += len + (p[len] == '\037');
        }
        memcpy(a->options, kept, sizeof kept);
        option(w, a->style);
    } else if (is(&w->l, "pure") && w->env->truecolor) {
        title(w, "Prompt Colors");
        print_(w, "");
        mark(w, 1);
        struct look l = now(w);
        for (int i = 0; i < 7; i++) SET(l.a.pure_color[i], pure_original[i]);
        labelled(w, "(1)  Original.", &l);
        for (int i = 0; i < 7; i++) SET(l.a.pure_color[i], pure_snazzy[i]);
        labelled(w, "(2)  Snazzy.", &l);
        print_(w, "(r)  Restart from the beginning.");
        char c = ask(w, "12r");
        if (!c) return R_QUIT;
        if (c == 'r') return R_RESTART;
        for (int i = 0; i < 7; i++) SET(a->pure_color[i], c == '1' ? pure_original[i] : pure_snazzy[i]);
        option(w, c == '1' ? "original" : "snazzy");
    }
    return R_OK;
}

static void lower(char *out, size_t cap, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

static int ask_color(struct wiz *w) {
    if (!is(&w->l, "classic")) return R_OK;
    title(w, "Prompt Color");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    for (int i = 0; i < 4; i++) {
        char label[48];
        snprintf(label, sizeof label, "(%d)  %s.", i + 1, w->color_name[i]);
        l.a.color = i + 1;
        labelled(w, label, &l);
    }
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "1234r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    w->l.a.color = c - '0';
    char name[32];
    lower(name, sizeof name, w->color_name[w->l.a.color - 1]);
    option(w, name);
    return R_OK;
}

static int ask_ornaments_color(struct wiz *w) {
    struct wizard_answers *a = &w->l.a;
    if (!(is(&w->l, "rainbow") || lean_any(&w->l)) || a->num_lines == 1) return R_OK;
    if (!strcmp(a->gap_char, " ") && !a->left_frame && !a->right_frame) return R_OK;
    char t[64] = "";
    if (strcmp(a->gap_char, " ")) strcat(t, "Connection");
    if (a->left_frame || a->right_frame) strcat(t, t[0] ? " & Frame" : "Frame");
    strcat(t, " Color");
    title(w, t);
    struct look l = now(w);
    char label[48];
    snprintf(label, sizeof label, "%%B(1)  %s.%%b", w->color_name[0]);
    l.a.color = 1;
    if (a->left_frame || a->right_frame) {
        add(w, 0, W_FRAME_MARKER, NULL);
        add(w, 3, W_PRINT, label);
        add_prompt_n(w, &l);
        print_(w, "");
        mark(w, 2);
    } else {
        mark(w, 1);
        print_(w, label);
        add_prompt(w, &l);
    }
    for (int i = 1; i < 4; i++) {
        snprintf(label, sizeof label, "(%d)  %s.", i + 1, w->color_name[i]);
        l.a.color = i + 1;
        labelled(w, label, &l);
    }
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "1234r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    a->color = c - '0';
    char name[48];
    lower(name, sizeof name - 12, w->color_name[a->color - 1]);
    strcat(name, "-ornaments");
    option(w, name);
    return R_OK;
}

static int ask_time(struct wiz *w) {
    title(w, "Show current time?");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    l.a.time[0] = 0;
    labelled(w, "(n)  No.", &l);
    SET(l.a.time, TIME_12H);
    labelled(w, "(1)  12-hour format.", &l);
    SET(l.a.time, TIME_24H);
    labelled(w, "(2)  24-hour format.", &l);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "n12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    if (c == 'n') w->l.a.time[0] = 0;
    else if (c == '1') { SET(w->l.a.time, TIME_12H); option(w, "12h time"); }
    else { SET(w->l.a.time, TIME_24H); option(w, "24h time"); }
    return R_OK;
}

static int ask_use_rprompt(struct wiz *w) {
    if (!is(&w->l, "pure")) return R_OK;
    title(w, "Non-permanent content location");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    labelled(w, "(1)  Left.", &l);
    l.a.pure_use_rprompt = true;
    labelled(w, "(2)  Right.", &l);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    if (c == '2') { w->l.a.pure_use_rprompt = true; option(w, "rprompt"); }
    return R_OK;
}

static int ask_extra_icons(struct wiz *w) {
    if (is(&w->l, "pure") || mode_is(w, "powerline") || mode_is(w, "compatible") || mode_is(w, "ascii")) return R_OK;
    /* os_icon_name: EmbLinkOS is no OS p10k has an icon for */
    struct sb dir = { 0 }, vcs = { 0 }, branch = { 0 }, duration = { 0 }, clock = { 0 };
    icon(w, "HOME_SUB_ICON", &dir);
    icon(w, "VCS_GIT_GITHUB_ICON", &vcs);
    icon_nospace(w, "VCS_BRANCH_ICON", &branch);
    icon(w, "EXECUTION_TIME_ICON", &duration);
    icon(w, "TIME_ICON", &clock);
    struct look few = now(w), many = now(w);
    for (int i = 0; i < 5; i++) few.a.extra_icons[i][0] = 0;
    many.a.extra_icons[0][0] = 0;
    snprintf(many.a.extra_icons[1], sizeof many.a.extra_icons[1], "%s ", sb_text(&dir));
    snprintf(many.a.extra_icons[2], sizeof many.a.extra_icons[2], "%s %s ", sb_text(&vcs), sb_text(&branch));
    snprintf(many.a.extra_icons[3], sizeof many.a.extra_icons[3], "%s ", sb_text(&duration));
    snprintf(many.a.extra_icons[4], sizeof many.a.extra_icons[4], "%s ", sb_text(&clock));
    sb_free(&dir); sb_free(&vcs); sb_free(&branch); sb_free(&duration); sb_free(&clock);
    title(w, "Icons");
    print_(w, "");
    mark(w, 1);
    labelled(w, "(1)  Few icons.", &few);
    labelled(w, "(2)  Many icons.", &many);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    memcpy(w->l.a.extra_icons, (c == '1' ? &few : &many)->a.extra_icons, sizeof w->l.a.extra_icons);
    option(w, c == '1' ? "few icons" : "many icons");
    return R_OK;
}

static int ask_prefixes(struct wiz *w) {
    if (is(&w->l, "pure")) return R_OK;
    struct look concise = now(w), fluent = now(w);
    static const char *const words[3] = { "on ", "took ", "at " };
    for (int i = 0; i < 3; i++) {
        concise.a.prefixes[i][0] = 0;
        SET(fluent.a.prefixes[i], words[i]);
    }
    title(w, "Prompt Flow");
    print_(w, "");
    mark(w, 1);
    labelled(w, "(1)  Concise.", &concise);
    labelled(w, "(2)  Fluent.", &fluent);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    memcpy(w->l.a.prefixes, (c == '1' ? &concise : &fluent)->a.prefixes, sizeof w->l.a.prefixes);
    option(w, c == '1' ? "concise" : "fluent");
    return R_OK;
}

static void seps(struct look *l, const char *ls, const char *rs, const char *lss, const char *rss) {
    SET(l->a.left_sep, ls);
    SET(l->a.right_sep, rs);
    SET(l->a.left_subsep, lss);
    SET(l->a.right_subsep, rss);
}

static int ask_separators(struct wiz *w) {
    if (!(is(&w->l, "classic") || is(&w->l, "rainbow")) || w->cap_diamond != 1) return R_OK;
    title(w, "Prompt Separators");
    add(w, 0, W_LINES, "              separator\n%B(1)  Angled.%b /\n            /");
    add(w, 3, W_PRINT, "%B(1)  Angled.%b");
    struct look l = now(w);
    seps(&l, RIGHT_TRIANGLE, LEFT_TRIANGLE, RIGHT_ANGLE, LEFT_ANGLE);
    add_prompt_n(w, &l);
    print_(w, "");
    mark(w, 2);
    seps(&l, "", "", VERTICAL_BAR, VERTICAL_BAR);
    labelled(w, "(2)  Vertical.", &l);
    if (nerdfont(w)) {
        seps(&l, w->down_triangle, w->up_triangle, w->slanted_bar, w->slanted_bar);
        labelled(w, "(3)  Slanted.", &l);
        seps(&l, RIGHT_CIRCLE, LEFT_CIRCLE, RIGHT_ARC, LEFT_ARC);
        labelled(w, "(4)  Round.", &l);
    }
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, nerdfont(w) ? "1234r" : "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    switch (c) {
    case '1': seps(&w->l, RIGHT_TRIANGLE, LEFT_TRIANGLE, RIGHT_ANGLE, LEFT_ANGLE); option(w, "angled separators"); break;
    case '2': seps(&w->l, "", "", VERTICAL_BAR, VERTICAL_BAR); option(w, "vertical separators"); break;
    case '3': seps(&w->l, w->down_triangle, w->up_triangle, w->slanted_bar, w->slanted_bar);
              option(w, "slanted separators"); break;
    default: seps(&w->l, RIGHT_CIRCLE, LEFT_CIRCLE, RIGHT_ARC, LEFT_ARC); option(w, "round separators"); break;
    }
    return R_OK;
}

static void ends(char *l, size_t lc, char *r, size_t rc, const char *lv, const char *rv) {
    snprintf(l, lc, "%s", lv);
    snprintf(r, rc, "%s", rv);
}
#define HEADS(look, lv, rv) ends((look)->a.left_head, sizeof (look)->a.left_head, (look)->a.right_head, \
                                 sizeof (look)->a.right_head, lv, rv)
#define TAILS(look, lv, rv) ends((look)->a.left_tail, sizeof (look)->a.left_tail, (look)->a.right_tail, \
                                 sizeof (look)->a.right_tail, lv, rv)

static int ask_heads(struct wiz *w) {
    if (!(is(&w->l, "classic") || is(&w->l, "rainbow")) || mode_is(w, "ascii")) return R_OK;
    title(w, "Prompt Heads");
    struct look l = now(w);
    if (w->cap_diamond) {
        add(w, 0, W_LINES, "                   head\n%B(1)  Sharp.%b         |\n                    v");
        add(w, 3, W_PRINT, "%B(1)  Sharp.%b");
        HEADS(&l, RIGHT_TRIANGLE, LEFT_TRIANGLE);
        add_prompt_n(w, &l);
        print_(w, "");
        mark(w, 2);
    } else {
        print_(w, "");
        mark(w, 1);
        HEADS(&l, "", "");
        labelled(w, "(1)  Flat.", &l);
    }
    HEADS(&l, FADE_OUT, FADE_IN);
    labelled(w, "(2)  Blurred.", &l);
    if (nerdfont(w)) {
        HEADS(&l, w->down_triangle, w->up_triangle);
        labelled(w, "(3)  Slanted.", &l);
        HEADS(&l, RIGHT_CIRCLE, LEFT_CIRCLE);
        labelled(w, "(4)  Round.", &l);
    }
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, nerdfont(w) ? "1234r" : "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    switch (c) {
    case '1':
        if (w->cap_diamond) { HEADS(&w->l, RIGHT_TRIANGLE, LEFT_TRIANGLE); option(w, "sharp heads"); }
        else { HEADS(&w->l, "", ""); option(w, "flat heads"); }
        break;
    case '2': HEADS(&w->l, FADE_OUT, FADE_IN); option(w, "blurred heads"); break;
    case '3': HEADS(&w->l, w->down_triangle, w->up_triangle); option(w, "slanted heads"); break;
    default: HEADS(&w->l, RIGHT_CIRCLE, LEFT_CIRCLE); option(w, "round heads"); break;
    }
    return R_OK;
}

static int ask_tails(struct wiz *w) {
    if (!(is(&w->l, "classic") || is(&w->l, "rainbow")) || mode_is(w, "ascii")) return R_OK;
    title(w, "Prompt Tails");
    add(w, 0, W_TAIL_MARKER, NULL);
    add(w, 3, W_PRINT, "%B(1)  Flat.%b");
    struct look l = now(w);
    TAILS(&l, "", "");
    add_prompt_n(w, &l);
    print_(w, "");
    mark(w, 2);
    TAILS(&l, FADE_IN, FADE_OUT);
    labelled(w, "(2)  Blurred.", &l);
    char choices[8] = "12";
    if (w->cap_diamond) {
        strcat(choices, "3");
        TAILS(&l, LEFT_TRIANGLE, RIGHT_TRIANGLE);
        labelled(w, "(3)  Sharp.", &l);
        if (nerdfont(w)) {
            strcat(choices, "45");
            TAILS(&l, w->up_triangle, w->down_triangle);
            labelled(w, "(4)  Slanted.", &l);
            TAILS(&l, LEFT_CIRCLE, RIGHT_CIRCLE);
            labelled(w, "(5)  Round.", &l);
        }
    }
    strcat(choices, "r");
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, choices);
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    switch (c) {
    case '1': TAILS(&w->l, "", ""); option(w, "flat tails"); break;
    case '2': TAILS(&w->l, FADE_IN, FADE_OUT); option(w, "blurred tails"); break;
    case '3': TAILS(&w->l, LEFT_TRIANGLE, RIGHT_TRIANGLE); option(w, "sharp tails"); break;
    case '4': TAILS(&w->l, w->up_triangle, w->down_triangle); option(w, "slanted tails"); break;
    default: TAILS(&w->l, LEFT_CIRCLE, RIGHT_CIRCLE); option(w, "round tails"); break;
    }
    return R_OK;
}

static int ask_num_lines(struct wiz *w) {
    title(w, "Prompt Height");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    l.a.num_lines = 1;
    labelled(w, "(1)  One line.", &l);
    l.a.num_lines = 2;
    labelled(w, "(2)  Two lines.", &l);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    w->l.a.num_lines = c - '0';
    option(w, c == '1' ? "1 line" : "2 lines");
    return R_OK;
}

static int ask_gap_char(struct wiz *w) {
    if (w->l.a.num_lines != 2 || is(&w->l, "pure")) return R_OK;
    bool ascii = mode_is(w, "ascii");
    const char *dot = ascii ? "." : "\xc2\xb7", *dash = ascii ? "-" : H;    /* · ─ */
    title(w, "Prompt Connection");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    SET(l.a.gap_char, " ");
    labelled(w, "(1)  Disconnected.", &l);
    SET(l.a.gap_char, dot);
    labelled(w, "(2)  Dotted.", &l);
    SET(l.a.gap_char, dash);
    labelled(w, "(3)  Solid.", &l);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "123r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    SET(w->l.a.gap_char, c == '1' ? " " : c == '2' ? dot : dash);
    option(w, c == '1' ? "disconnected" : c == '2' ? "dotted" : "solid");
    return R_OK;
}

static int ask_frame(struct wiz *w) {
    if (!(is(&w->l, "classic") || is(&w->l, "rainbow") || lean_any(&w->l)) || w->l.a.num_lines != 2 ||
        mode_is(w, "ascii"))
        return R_OK;
    static const char *const labels[4] = { "(1)  No frame.", "(2)  Left.", "(3)  Right.", "(4)  Full." };
    static const char *const names[4] = { "no frame", "left frame", "right frame", "full frame" };
    title(w, "Prompt Frame");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    for (int i = 0; i < 4; i++) {
        l.a.left_frame = i == 1 || i == 3;
        l.a.right_frame = i >= 2;
        labelled(w, labels[i], &l);
    }
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "1234r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    int i = c - '1';
    w->l.a.left_frame = i == 1 || i == 3;
    w->l.a.right_frame = i >= 2;
    option(w, names[i]);
    return R_OK;
}

static int ask_empty_line(struct wiz *w) {
    title(w, "Prompt Spacing");
    print_(w, "");
    mark(w, 1);
    struct look l = now(w);
    print_(w, "%B(1)  Compact.%b");
    print_(w, "");
    mark(w, 1);
    add_prompt_n(w, &l);
    add_prompt_n(w, &l);
    print_(w, "");
    mark(w, 2);
    print_(w, "%B(2)  Sparse.%b");
    print_(w, "");
    mark(w, 1);
    add_prompt_n(w, &l);
    print_(w, "");
    add_prompt_n(w, &l);
    print_(w, "");
    mark(w, 2);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "12r");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    w->l.a.empty_line = c == '2';
    option(w, c == '1' ? "compact" : "sparse");
    return R_OK;
}

static int ask_transient_prompt(struct wiz *w) {
    struct wizard_answers *a = &w->l.a;
    struct look l = now(w);
    l.disable_rprompt = a->num_lines == 1;
    char p[32], line[96];
    snprintf(p, sizeof p, "%%F{%s}%s%%f", is(&w->l, "pure") ? a->pure_color[MAGENTA] : is(&w->l, "lean_8colors") ? "2" : "76",
             w->l.prompt_char);
    title(w, "Enable Transient Prompt?");
    print_(w, "");
    mark(w, 1);
    print_(w, "%B(y)  Yes.%b");
    print_(w, "");
    mark(w, 1);
    snprintf(line, sizeof line, "  %s %%2Fgit%%f pull", p);
    print_(w, line);
    mark(w, 3);
    snprintf(line, sizeof line, "  %s %%2Fgit%%f branch x", p);
    print_(w, line);
    if (a->empty_line) print_(w, "");
    SET(l.buffer, "%2Fgit%f checkout x" CURSOR);
    add_prompt_n(w, &l);
    print_(w, "");
    mark(w, 2);
    print_(w, "%B(n)  No.%b");
    print_(w, "");
    mark(w, 1);
    SET(l.buffer, "%2Fgit%f pull");
    struct widget *x = add(w, 0, W_PROMPT, NULL);
    if (x && (x->look = malloc(sizeof l))) *x->look = l;
    mark(w, 3);
    if (a->empty_line) { print_(w, ""); mark(w, 3); }
    SET(l.buffer, "%2Fgit%f branch x");
    add_prompt_n(w, &l);
    if (a->empty_line) print_(w, "");
    SET(l.buffer, "%2Fgit%f checkout x" CURSOR);
    add_prompt_n(w, &l);
    print_(w, "");
    mark(w, 2);
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "ynr");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    a->transient = c == 'y';
    if (a->transient) option(w, "transient_prompt");
    return R_OK;
}

static int ask_config_overwrite(struct wiz *w) {
    w->l.a.overwrite = false;
    if (!w->env->exists) return R_OK;
    flow_c(w, "Powerlevel10k config file already exists.");
    struct sb s = { 0 };
    sb_str(&s, "%BOverwrite %b%2F");
    for (const char *p = w->env->file; *p; p++) {
        if (*p == '\\') sb_str(&s, "\\\\");
        else if (*p == ' ') sb_str(&s, NB);
        else sb_chr(&s, *p);
    }
    sb_str(&s, "%f%B?%b");
    flow_c(w, sb_text(&s));
    sb_free(&s);
    print_(w, "");
    print_(w, "%B(y)  Yes.%b");
    print_(w, "");
    print_(w, "(r)  Restart from the beginning.");
    char c = ask(w, "yr");
    if (!c) return R_QUIT;
    if (c == 'r') return R_RESTART;
    w->l.a.overwrite = true;
    return R_OK;
}

/* --- the wizard ---------------------------------------------------------------- */

static void fresh(struct wiz *w) {
    struct wizard_answers *a = &w->l.a;
    memset(&w->l, 0, sizeof w->l);
    SET(a->instant_prompt, "verbose");
    SET(a->icon_padding, "moderate");
    SET(a->gap_char, " ");
    SET(w->l.prompt_char, "\xe2\x9d\xaf");                     /* ❯ */
    SET(w->down_triangle, "\\uE0BC");
    SET(w->up_triangle, "\\uE0BA");
    SET(w->slanted_bar, "\\u2571");
    a->num_lines = 2;
    a->color = 2;
    a->left_frame = a->right_frame = 1;
    w->cap_diamond = w->cap_lock = w->cap_arrow = w->cap_quotes = 0;
    static const int frame[4] = { 244, 242, 240, 238 };
    static const char *const names[4] = { "Lightest", "Light", "Dark", "Darkest" };
    memcpy(a->frame_color, frame, sizeof frame);
    memcpy(a->bg_color, bg_color, sizeof bg_color);
    memcpy(a->sep_color, sep_color, sizeof sep_color);
    memcpy(a->prefix_color, prefix_color, sizeof prefix_color);
    memcpy(w->color_name, names, sizeof names);
    for (int i = 0; i < 7; i++) SET(a->pure_color[i], w->env->truecolor ? pure_snazzy[i] : pure_original[i]);
}

/* The questions about the font, and the mode they say it can do. */
static int ask_font(struct wiz *w) {
    struct wizard_answers *a = &w->l.a;
    int r;
    if ((r = ask_diamond(w)) != R_OK) return r;
    if ((r = ask_lock(w, "\\uF023", NULL)) != R_OK) return r;
    if (!w->cap_lock) {
        if ((r = ask_lock(w, "\\uE138", "Let's" NB "try" NB "another" NB "one.")) != R_OK) return r;
        if (w->cap_lock) {
            if (w->cap_diamond) {
                SET(a->mode, "awesome-patched");
                if ((r = ask_python(w)) != R_OK) return r;
            } else SET(a->mode, "flat");
        } else if (w->cap_diamond) SET(a->mode, "powerline");
        else {
            if ((r = ask_quotes(w)) != R_OK) return r;
            SET(a->mode, w->cap_quotes ? "compatible" : "ascii");
        }
    } else if (!w->cap_diamond) SET(a->mode, "awesome-fontconfig");
    else {
        if ((r = ask_arrow(w, "\\uFC35", NULL)) != R_OK) return r;
        if (w->cap_arrow) SET(a->mode, "nerdfont-complete");
        else {
            if ((r = ask_arrow(w, "\\UF0737", "Let's" NB "try" NB "another" NB "one.")) != R_OK) return r;
            if (w->cap_arrow && (r = ask_width(w)) != R_OK) return r;
            if (w->cap_arrow) SET(a->mode, "nerdfont-v3");
            else {
                SET(a->mode, "awesome-fontconfig");
                if ((r = ask_python(w)) != R_OK) return r;
            }
        }
    }
    return R_OK;
}

static void restore_screen(struct wiz *w, bool *on) {
    if (*on) put(w, "\033[?1049l");
    *on = false;
}

static void consume_input(struct wiz *w) {
    w->npending = 0;
    while (w->io->key(w->io->ctx, 0) >= 0) {}
}

/* quit: how to run it again, in the main screen. */
static void quit(struct wiz *w, bool *screen) {
    consume_input(w);
    restore_screen(w, screen);
    struct out o = { { 0 }, { "", "", false, false, false } }, *s = &o;
    sb_chr(&o.s, '\n');
    if (w->env->force) {
        flowing(s, w->cols, false, 0, "Powerlevel10k configuration wizard has been aborted. To run it again, type:");
        println(s, "");
        println(s, "  %2Fprompt%f %Bconfigure%b");
        println(s, "");
    } else {
        struct sb t = { 0 };
        sb_str(&t, "Powerlevel10k configuration wizard has been aborted. It will run again next time unless you "
                   "define at least one Powerlevel10k configuration option. To define an option that does nothing "
                   "except for disabling Powerlevel10k configuration wizard, add this line to %B");
        sb_str(&t, w->env->rc);
        sb_str(&t, "%b:");
        flowing(s, w->cols, false, 0, sb_text(&t));
        sb_free(&t);
        println(s, "");
        println(s, "  %2Fprompt%f set disable.configuration.wizard %3Ftrue%f");
        println(s, "");
        flowing(s, w->cols, false, 0, "To run Powerlevel10k configuration wizard right now, type:");
        println(s, "");
        println(s, "  %2Fprompt%f %Bconfigure%b");
        println(s, "");
    }
    put_text(w, sb_text(&o.s));
    sb_free(&o.s);
    put(w, "\033[?25h");
}

enum wizard_end wizard_run(const struct wizard_io *io, const struct wizard_env *env, struct wizard_answers *out) {
    struct wiz *w = calloc(1, sizeof *w);
    if (!w) return WIZARD_QUIT;
    w->io = io;
    w->env = env;
    w->cols = MAX_COLUMNS;
    bool screen = true;
    put(w, "\033[?1049h");                                     /* smcup */
    int r;
    for (;;) {
        fresh(w);
        struct wizard_answers *a = &w->l.a;
        if ((r = ask_font(w)) == R_RESTART) continue;
        if (r == R_QUIT) break;
        if (!strcmp(a->mode, "powerline")) option(w, "powerline");
        else {
            char opt[64];
            snprintf(opt, sizeof opt, "%s%s%s", a->mode, w->cap_diamond ? " + powerline" : "", a->cap_python ? " + python" : "");
            option(w, opt);
        }
        if (w->cap_diamond) {
            seps(&w->l, RIGHT_TRIANGLE, LEFT_TRIANGLE, RIGHT_ANGLE, LEFT_ANGLE);
            HEADS(&w->l, RIGHT_TRIANGLE, LEFT_TRIANGLE);
        } else {
            a->left_sep[0] = a->right_sep[0] = a->left_head[0] = a->right_head[0] = 0;
            if (mode_is(w, "ascii")) ascii_look(&w->l);
            else { SET(a->left_subsep, VERTICAL_BAR); SET(a->right_subsep, VERTICAL_BAR); }
        }
        static int (*const questions[])(struct wiz *) = {
            ask_icon_padding, ask_style, ask_charset, ask_color_scheme, ask_color, ask_use_rprompt, ask_time,
            ask_separators, ask_heads, ask_tails, ask_num_lines, ask_gap_char, ask_frame, ask_ornaments_color,
            ask_empty_line, ask_extra_icons, ask_prefixes, ask_transient_prompt, ask_config_overwrite };
        r = R_OK;
        for (size_t i = 0; r == R_OK && i < sizeof questions / sizeof *questions; i++) r = questions[i](w);
        if (r == R_RESTART) { clear_widgets(w); continue; }
        break;
    }
    clear_widgets(w);
    if (r == R_QUIT) {
        quit(w, &screen);
        free(w);
        return WIZARD_QUIT;
    }
    /* the mode's branch icon, for VCS_BRANCH_ICON, as icons.zsh writes it */
    struct sb b = { 0 };
    icon_nospace(w, "VCS_BRANCH_ICON", &b);
    char *e = w->l.a.branch_icon;
    size_t en = 0;
    for (const char *p = sb_text(&b); *p && en + 8 < sizeof w->l.a.branch_icon;) {
        uint32_t cp;
        int len = utf8_decode(p, (int)strlen(p), &cp);
        if (cp < 0x80) e[en++] = (char)cp;
        else en += (size_t)snprintf(e + en, sizeof w->l.a.branch_icon - en, cp > 0xffff ? "\\U%X" : "\\u%04X", cp);
        p += len;
    }
    e[en] = 0;
    sb_free(&b);
    consume_input(w);
    restore_screen(w, &screen);
    int cols, rows;
    size_now(w, &cols, &rows);
    struct out o = { { 0 }, { "", "", false, false, false } };
    struct sb t = { 0 };
    sb_chr(&o.s, '\n');
    sb_str(&t, "New config: %U");
    sb_str(&t, env->file);
    sb_str(&t, "%u.");
    flowing(&o, cols, false, 0, sb_text(&t));
    if (w->l.a.overwrite) {
        sb_reset(&t);
        sb_str(&t, "Backup of the old config: %U");
        sb_str(&t, env->backup);
        sb_str(&t, "%u.");
        flowing(&o, cols, false, 0, sb_text(&t));
    }
    sb_chr(&o.s, '\n');
    put_text(w, sb_text(&o.s));
    sb_free(&o.s);
    sb_free(&t);
    *out = w->l.a;
    free(w);
    return WIZARD_DONE;
}

/* --- the configuration ---------------------------------------------------------- */

void wizard_template(const struct wizard_answers *a, char *out, size_t cap) {
    snprintf(out, cap, "%s", a->style);
    for (char *p = out; *p; p++) if (*p == '_') *p = '-';
}

struct lines { char **v; size_t n, cap; };

static bool line_push(struct lines *ls, const char *s, size_t n) {
    if (ls->n == ls->cap) {
        size_t cap = ls->cap ? ls->cap * 2 : 1024;
        char **v = realloc(ls->v, cap * sizeof *v);
        if (!v) return false;
        ls->v = v;
        ls->cap = cap;
    }
    char *c = malloc(n + 1);
    if (!c) return false;
    memcpy(c, s, n);
    c[n] = 0;
    ls->v[ls->n++] = c;
    return true;
}
static void line_set(struct lines *ls, size_t i, const char *s) {
    char *c = malloc(strlen(s) + 1);
    if (!c) return;
    strcpy(c, s);
    free(ls->v[i]);
    ls->v[i] = c;
}

/* sub NAME VALUE: `typeset -g POWERLEVEL9K_NAME=...` lines say VALUE. */
static void sub(struct lines *ls, const char *name, const char *value) {
    char head[160];
    snprintf(head, sizeof head, "typeset -g POWERLEVEL9K_%s=", name);
    for (size_t i = 0; i < ls->n; i++) {
        const char *l = ls->v[i];
        size_t ws = strspn(l, " \t");
        if (strncmp(l + ws, head, strlen(head))) continue;
        struct sb s = { 0 };
        sb_add(&s, l, ws);
        sb_str(&s, head);
        sb_str(&s, value);
        line_set(ls, i, sb_text(&s));
        sb_free(&s);
    }
}

/* uncomment TEXT: `# TEXT` lines lose the `# `, and keep their columns. */
static void uncomment(struct lines *ls, const char *text) {
    for (size_t i = 0; i < ls->n; i++) {
        const char *l = ls->v[i];
        size_t ws = strspn(l, " \t"), tl = strlen(text);
        if (strncmp(l + ws, "# ", 2) || strncmp(l + ws + 2, text, tl)) continue;
        const char *rest = l + ws + 2 + tl;
        bool two = !strncmp(rest, "  ", 2);
        struct sb s = { 0 };
        sb_add(&s, l, ws);
        sb_str(&s, text);
        if (two) { sb_str(&s, "    "); rest += 2; }
        sb_str(&s, rest);
        line_set(ls, i, sb_text(&s));
        sb_free(&s);
    }
}

/* rep FROM TO: every FROM in every line is TO. */
static void rep(struct lines *ls, const char *from, const char *to) {
    size_t fl = strlen(from);
    for (size_t i = 0; i < ls->n; i++) {
        if (!strstr(ls->v[i], from)) continue;
        struct sb s = { 0 };
        for (const char *p = ls->v[i]; *p;) {
            if (!strncmp(p, from, fl)) { sb_str(&s, to); p += fl; }
            else sb_chr(&s, *p++);
        }
        line_set(ls, i, sb_text(&s));
        sb_free(&s);
    }
}

static void subf(struct lines *ls, const char *name, const char *fmt, ...) {
    char v[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    sub(ls, name, v);
}

/* sum(1), as BSD has it: what p10k's header says the template was. */
static unsigned bsd_sum(const char *s) {
    unsigned c = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) c = ((c >> 1) + ((c & 1) << 15) + *p) & 0xffff;
    return c;
}

char *wizard_config(const struct wizard_answers *a, const char *base, const char *date) {
    struct lines ls = { 0 };
    /* "$(<file)": the trailing newlines go; "${(@f)...}": a line each */
    size_t bn = strlen(base);
    while (bn && base[bn - 1] == '\n') bn--;
    for (size_t i = 0; i <= bn;) {
        size_t len = 0;
        while (i + len < bn && base[i + len] != '\n') len++;
        if (!line_push(&ls, base + i, len)) goto oom;
        i += len + 1;
    }
    bool pure = !strcmp(a->style, "pure");
    int fc = a->frame_color[a->color - 1];
    if (pure) {
        static const char *const names[7] = { "grey", "red", "yellow", "blue", "magenta", "cyan", "white" };
        static const char *const was[7] = { "242", "1", "3", "4", "5", "6", "7" };
        for (int i = 0; i < 7; i++) {
            char from[32], to[48];
            snprintf(from, sizeof from, "local %s=%s", names[i], was[i]);
            snprintf(to, sizeof to, "local %s='%s'", names[i], a->pure_color[i]);
            rep(&ls, from, to);
        }
    } else {
        const char *mode = a->mode;
        sub(&ls, "MODE", mode);
        sub(&ls, "ICON_PADDING", a->icon_padding);
        if (!strcmp(mode, "compatible")) {
            sub(&ls, "STATUS_ERROR_VISUAL_IDENTIFIER_EXPANSION", "'\xd1\x85'");             /* х */
            sub(&ls, "STATUS_ERROR_SIGNAL_VISUAL_IDENTIFIER_EXPANSION", "'\xd1\x85'");
            sub(&ls, "STATUS_ERROR_PIPE_VISUAL_IDENTIFIER_EXPANSION", "'\xd1\x85'");
        }
        if (!strcmp(mode, "compatible") || !strcmp(mode, "powerline")) {
            static const char *const pairs[][2] = {
                { "LOCK_ICON", "'\xe2\x88\x85'" },                                           /* ∅ */
                { "NORDVPN_VISUAL_IDENTIFIER_EXPANSION", "'nord'" },
                { "RANGER_VISUAL_IDENTIFIER_EXPANSION", "'\xe2\x96\xb2'" },                  /* ▲ */
                { "KUBECONTEXT_DEFAULT_VISUAL_IDENTIFIER_EXPANSION", "'\xe2\x97\x8b'" },     /* ○ */
                { "AZURE_VISUAL_IDENTIFIER_EXPANSION", "'az'" },
                { "AWS_EB_ENV_VISUAL_IDENTIFIER_EXPANSION", "'eb'" },
                { "BACKGROUND_JOBS_VISUAL_IDENTIFIER_EXPANSION", "'\xe2\x89\xa1'" } };      /* ≡ */
            for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
                char t[96];
                snprintf(t, sizeof t, "typeset -g POWERLEVEL9K_%s", pairs[i][0]);
                uncomment(&ls, t);
                sub(&ls, pairs[i][0], pairs[i][1]);
            }
        }
        if ((!strcmp(mode, "awesome-patched") || !strcmp(mode, "awesome-fontconfig")) && !a->cap_python) {
            static const char *const names[] = { "VIRTUALENV_VISUAL_IDENTIFIER_EXPANSION", "ANACONDA_VISUAL_IDENTIFIER_EXPANSION",
                                                 "PYENV_VISUAL_IDENTIFIER_EXPANSION", "PYTHON_ICON" };
            for (size_t i = 0; i < 4; i++) {
                char t[96];
                snprintf(t, sizeof t, "typeset -g POWERLEVEL9K_%s", names[i]);
                uncomment(&ls, t);
            }
            for (size_t i = 0; i < 4; i++) sub(&ls, names[i], "'\xf0\x9f\x90\x8d'");    /* 🐍 */
        }
        if (!strcmp(mode, "nerdfont-complete"))
            sub(&ls, "BATTERY_STAGES", "'\\uf58d\\uf579\\uf57a\\uf57b\\uf57c\\uf57d\\uf57e\\uf57f\\uf580\\uf581\\uf578'");
        else if (!strcmp(mode, "nerdfont-v3"))
            sub(&ls, "BATTERY_STAGES", "'\\UF008E\\UF007A\\UF007B\\UF007C\\UF007D\\UF007E\\UF007F\\UF0080\\UF0081\\UF0082\\UF0079'");
        bool classic = !strcmp(a->style, "classic"), rainbow = !strcmp(a->style, "rainbow");
        if (classic || rainbow) {
            if (classic) {
                int c = a->color - 1;
                subf(&ls, "BACKGROUND", "%d", a->bg_color[c]);
                subf(&ls, "LEFT_SUBSEGMENT_SEPARATOR", "'%%%dF%s'", a->sep_color[c], a->left_subsep);
                subf(&ls, "RIGHT_SUBSEGMENT_SEPARATOR", "'%%%dF%s'", a->sep_color[c], a->right_subsep);
                subf(&ls, "VCS_LOADING_FOREGROUND", "%d", a->sep_color[c]);
                char to[16];
                snprintf(to, sizeof to, "%%%dF", a->prefix_color[c]);
                rep(&ls, "%248F", to);
            } else {
                subf(&ls, "LEFT_SUBSEGMENT_SEPARATOR", "'%s'", a->left_subsep);
                subf(&ls, "RIGHT_SUBSEGMENT_SEPARATOR", "'%s'", a->right_subsep);
            }
            subf(&ls, "RULER_FOREGROUND", "%d", fc);
            subf(&ls, "MULTILINE_FIRST_PROMPT_GAP_FOREGROUND", "%d", fc);
            subf(&ls, "MULTILINE_FIRST_PROMPT_PREFIX", "'%%%dF" TL H "'", fc);
            subf(&ls, "MULTILINE_NEWLINE_PROMPT_PREFIX", "'%%%dF" ML H "'", fc);
            subf(&ls, "MULTILINE_LAST_PROMPT_PREFIX", "'%%%dF" BL H "'", fc);
            subf(&ls, "MULTILINE_FIRST_PROMPT_SUFFIX", "'%%%dF" H TR "'", fc);
            subf(&ls, "MULTILINE_NEWLINE_PROMPT_SUFFIX", "'%%%dF" H MR "'", fc);
            subf(&ls, "MULTILINE_LAST_PROMPT_SUFFIX", "'%%%dF" H BR "'", fc);
            subf(&ls, "LEFT_SEGMENT_SEPARATOR", "'%s'", a->left_sep);
            subf(&ls, "RIGHT_SEGMENT_SEPARATOR", "'%s'", a->right_sep);
            subf(&ls, "LEFT_PROMPT_FIRST_SEGMENT_START_SYMBOL", "'%s'", a->left_tail);
            subf(&ls, "LEFT_PROMPT_LAST_SEGMENT_END_SYMBOL", "'%s'", a->left_head);
            subf(&ls, "RIGHT_PROMPT_FIRST_SEGMENT_START_SYMBOL", "'%s'", a->right_head);
            subf(&ls, "RIGHT_PROMPT_LAST_SEGMENT_END_SYMBOL", "'%s'", a->right_tail);
        }
        bool icons = false;
        for (int i = 0; i < 5; i++) icons |= a->extra_icons[i][0] != 0;
        if (icons) {
            subf(&ls, "VCS_BRANCH_ICON", "'%s '", a->branch_icon);
            uncomment(&ls, "os_icon");
        } else {
            uncomment(&ls, "typeset -g POWERLEVEL9K_DIR_CLASSES");
            uncomment(&ls, "typeset -g POWERLEVEL9K_VCS_VISUAL_IDENTIFIER_EXPANSION");
            uncomment(&ls, "typeset -g POWERLEVEL9K_COMMAND_EXECUTION_TIME_VISUAL_IDENTIFIER_EXPANSION");
            uncomment(&ls, "typeset -g POWERLEVEL9K_TIME_VISUAL_IDENTIFIER_EXPANSION");
            sub(&ls, "VCS_VISUAL_IDENTIFIER_EXPANSION", "");
            sub(&ls, "COMMAND_EXECUTION_TIME_VISUAL_IDENTIFIER_EXPANSION", "");
            sub(&ls, "TIME_VISUAL_IDENTIFIER_EXPANSION", "");
        }
        bool prefixes = false;
        for (int i = 0; i < 3; i++) prefixes |= a->prefixes[i][0] != 0;
        if (prefixes) {
            static const char *const names[] = { "VCS_PREFIX", "COMMAND_EXECUTION_TIME_PREFIX", "CONTEXT_PREFIX",
                                                 "KUBECONTEXT_PREFIX", "TIME_PREFIX", "TOOLBOX_PREFIX" };
            static const char *const words[] = { "on ", "took ", "with ", "at ", "at ", "in " };
            for (size_t i = 0; i < 6; i++) {
                char t[96];
                snprintf(t, sizeof t, "typeset -g POWERLEVEL9K_%s", names[i]);
                uncomment(&ls, t);
            }
            if (!strcmp(a->style, "lean") || classic) {
                char fg[16];
                if (classic) snprintf(fg, sizeof fg, "%%%dF", a->prefix_color[a->color - 1]);
                else snprintf(fg, sizeof fg, "%%f");
                for (size_t i = 0; i < 6; i++) subf(&ls, names[i], "'%s%s'", fg, words[i]);
            }
        }
        subf(&ls, "MULTILINE_FIRST_PROMPT_GAP_CHAR", "'%s'", a->gap_char);
        if ((classic || rainbow) && a->num_lines == 2) {
            if (!a->right_frame) {
                sub(&ls, "MULTILINE_FIRST_PROMPT_SUFFIX", "");
                sub(&ls, "MULTILINE_NEWLINE_PROMPT_SUFFIX", "");
                sub(&ls, "MULTILINE_LAST_PROMPT_SUFFIX", "");
            }
            if (!a->left_frame) {
                sub(&ls, "MULTILINE_FIRST_PROMPT_PREFIX", "");
                sub(&ls, "MULTILINE_NEWLINE_PROMPT_PREFIX", "");
                sub(&ls, "MULTILINE_LAST_PROMPT_PREFIX", "");
                sub(&ls, "STATUS_OK", "false");
                sub(&ls, "STATUS_ERROR", "false");
            }
        }
        if (!strncmp(a->style, "lean", 4)) {
            subf(&ls, "RULER_FOREGROUND", "%d", fc);
            subf(&ls, "MULTILINE_FIRST_PROMPT_GAP_FOREGROUND", "%d", fc);
            if (a->right_frame) {
                subf(&ls, "MULTILINE_FIRST_PROMPT_SUFFIX", "'%%%dF" H TR "'", fc);
                subf(&ls, "MULTILINE_NEWLINE_PROMPT_SUFFIX", "'%%%dF" H MR "'", fc);
                subf(&ls, "MULTILINE_LAST_PROMPT_SUFFIX", "'%%%dF" H BR "'", fc);
                sub(&ls, "RIGHT_PROMPT_LAST_SEGMENT_END_SYMBOL", "' '");
            }
            if (a->left_frame) {
                subf(&ls, "MULTILINE_FIRST_PROMPT_PREFIX", "'%%%dF" TL H "'", fc);
                subf(&ls, "MULTILINE_NEWLINE_PROMPT_PREFIX", "'%%%dF" ML H "'", fc);
                subf(&ls, "MULTILINE_LAST_PROMPT_PREFIX", "'%%%dF" BL H "'", fc);
                sub(&ls, "LEFT_PROMPT_FIRST_SEGMENT_START_SYMBOL", "' '");
            }
        }
        if (classic || rainbow) uncomment(&ls, a->num_lines == 2 && !a->left_frame ? "prompt_char" : "vi_mode");
        if (!strcmp(mode, "ascii")) {
            sub(&ls, "STATUS_OK_VISUAL_IDENTIFIER_EXPANSION", "'ok'");
            sub(&ls, "STATUS_OK_PIPE_VISUAL_IDENTIFIER_EXPANSION", "'ok'");
            sub(&ls, "STATUS_ERROR_VISUAL_IDENTIFIER_EXPANSION", "'err'");
            sub(&ls, "STATUS_ERROR_SIGNAL_VISUAL_IDENTIFIER_EXPANSION", "");
            sub(&ls, "STATUS_ERROR_PIPE_VISUAL_IDENTIFIER_EXPANSION", "'err'");
            sub(&ls, "BATTERY_STAGES", "('battery')");
            sub(&ls, "PROMPT_CHAR_{OK,ERROR}_VIINS_CONTENT_EXPANSION", "'>'");
            sub(&ls, "PROMPT_CHAR_{OK,ERROR}_VICMD_CONTENT_EXPANSION", "'<'");
            sub(&ls, "PROMPT_CHAR_{OK,ERROR}_VIVIS_CONTENT_EXPANSION", "'V'");
            sub(&ls, "PROMPT_CHAR_{OK,ERROR}_VIOWR_CONTENT_EXPANSION", "'^'");
            rep(&ls, "-i '\xe2\xad\x90'", "-i '*'");                      /* ⭐ */
            rep(&ls, "\xe2\x80\xa6", "..");                               /* … */
            rep(&ls, "\xe2\x87\xa3", "<");                                /* ⇣ */
            rep(&ls, "\xe2\x87\xa1", ">");                                /* ⇡ */
            rep(&ls, "\xe2\x87\xa0", "<-");                               /* ⇠ */
            rep(&ls, "\xe2\x87\xa2", "->");                               /* ⇢ */
            rep(&ls, H, "-");
        }
    }
    if (a->pure_use_rprompt) {
        static const char *const segments[] = { "command_execution_time", "virtualenv", "context" };
        for (size_t i = 0; i < 3; i++) {
            char from[64], to[64];
            snprintf(from, sizeof from, "    %s", segments[i]);
            snprintf(to, sizeof to, "    tmp_%s", segments[i]);
            rep(&ls, from, to);
            uncomment(&ls, segments[i]);
            snprintf(from, sizeof from, "    tmp_%s  ", segments[i]);
            snprintf(to, sizeof to, "    # %s", segments[i]);
            rep(&ls, from, to);
        }
    }
    if (a->time[0]) {
        uncomment(&ls, "time");
        if (!strcmp(a->time, TIME_12H)) sub(&ls, "TIME_FORMAT", "'%D{%I:%M:%S %p}'");
    }
    if (a->num_lines == 1) {
        size_t k = 0;
        for (size_t i = 0; i < ls.n; i++) {
            if (!strncmp(ls.v[i], "    newline", 11) || strstr(ls.v[i], "===[ Line #")) { free(ls.v[i]); continue; }
            ls.v[k++] = ls.v[i];
        }
        ls.n = k;
    }
    sub(&ls, "PROMPT_ADD_NEWLINE", a->empty_line ? "true" : "false");
    sub(&ls, "INSTANT_PROMPT", a->instant_prompt);
    if (a->transient) sub(&ls, "TRANSIENT_PROMPT", "always");

    struct sb out = { 0 }, sum = { 0 };
    char tmpl[24];
    wizard_template(a, tmpl, sizeof tmpl);
    sb_fmt(&out, "# Generated by Powerlevel10k configuration wizard on %s.\n", date);
    sb_fmt(&out, "# Based on romkatv/powerlevel10k/config/p10k-%s.zsh", tmpl);
    sb_add(&sum, base, bn);
    sb_chr(&sum, '\n');                                         /* sum <<<$base */
    sb_fmt(&out, ", checksum %u.\n", bsd_sum(sb_text(&sum)));
    sb_free(&sum);
    struct sb line = { 0 };
    bool first = true;
    for (const char *p = a->options; *p || first;) {
        size_t len = strcspn(p, "\037");
        if (first) { sb_str(&line, "# Wizard options: "); sb_add(&line, p, len); first = false; }
        else if (line.n + len > 85) {                          /* $#line + $#opt > 85, in characters */
            sb_str(&out, sb_text(&line));
            sb_str(&out, ",\n");
            sb_reset(&line);
            sb_str(&line, "# ");
            sb_add(&line, p, len);
        } else {
            sb_str(&line, ", ");
            sb_add(&line, p, len);
        }
        p += len;
        if (*p == '\037') p++;
        else break;
    }
    sb_str(&out, sb_text(&line));
    sb_str(&out, ".\n# Type `prompt configure` to generate another config.\n#\n");
    sb_free(&line);
    for (size_t i = 0; i < ls.n; i++) {
        sb_str(&out, ls.v[i]);
        sb_chr(&out, '\n');
        free(ls.v[i]);
    }
    free(ls.v);
    if (out.oom) { sb_free(&out); return NULL; }
    return out.p;
oom:
    for (size_t i = 0; i < ls.n; i++) free(ls.v[i]);
    free(ls.v);
    return NULL;
}
