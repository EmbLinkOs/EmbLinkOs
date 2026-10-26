/* ==========================================================================
 * p10k_import.c -- a ~/.p10k.zsh read into a configuration.
 *
 * What `p10k configure` writes, and what people add to it: `typeset -g
 * POWERLEVEL9K_...` with brace lists ({LEFT,RIGHT}_...), arrays across lines,
 * '...', "...", $'...', locals and ${(j:|:)array}, `if [[ ... ]]; then ... fi`
 * on parameters already set, and the my_git_formatter function, whose colours
 * are read into _GIT_FORMAT_* for the engine's own formatter (p10k_zsh.c).
 * Other functions are skipped, as are commands joined by && or ||: they are
 * zsh's business (option juggling, `p10k reload`), not the prompt's. It is a
 * reader for these files, not a zsh.
 * ========================================================================== */
#include "prompt/p10k_int.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unquoted { , } in a word, kept apart from quoted ones for brace expansion. */
#define BO '\021'
#define CM '\022'
#define BC '\023'
#define WSEP '\036'              /* between the words of a brace expansion */

enum tk { TK_WORD, TK_ARRAY, TK_COND, TK_ARITH, TK_LPAREN, TK_RPAREN, TK_SEP, TK_AND, TK_OR, TK_PIPE, TK_EOF };

struct tok {
    enum tk kind;
    struct sb text;              /* WORD: with markers; ARRAY: the name and = or +=; ARITH: the expression */
    struct sb elems;             /* ARRAY: elements, each ending in WSEP; COND: words, each 'q'/'u' + text + WSEP */
    bool quoted;
};

struct imp {
    struct p10k *cfg, *vars;     /* vars: locals and other globals */
    const char *s;
    size_t i, n;
    unsigned line;
    struct p10k_import *rep;
    bool failed;
};

static void fail(struct imp *m, const char *why) {
    if (m->failed) return;
    m->failed = true;
    if (m->rep) { m->rep->line = m->line; snprintf(m->rep->error, sizeof m->rep->error, "line %u: %s", m->line, why); }
}

static void tok_free(struct tok *t) { sb_free(&t->text); sb_free(&t->elems); }

/* --- values ------------------------------------------------------------------ */

/* $NAME's value: a local, a POWERLEVEL9K_ parameter, else nothing. Arrays
 * joined by `sep`. */
static void lookup(const struct imp *m, const char *name, const char *sep, struct sb *out) {
    const char *v = p10k_get(m->vars, name);
    if (!v && !strncmp(name, "POWERLEVEL9K_", 13)) v = p10k_get(m->cfg, name + 13);
    if (!v && !strcmp(name, "DEFAULT_USER")) v = p10k_get(m->cfg, "_DEFAULT_USER");
    if (!v) return;
    for (; *v; v++) { if (*v == P10K_ARRAY_SEP) sb_str(out, sep); else sb_chr(out, *v); }
}

/* After "$": NAME, {NAME}, {(j:x:)NAME}. Anything else is kept as written. */
static void dollar(struct imp *m, struct sb *out) {
    const char *s = m->s;
    size_t i = m->i;
    if (i < m->n && (isalpha((unsigned char)s[i]) || s[i] == '_')) {
        size_t e = i;
        while (e < m->n && (isalnum((unsigned char)s[e]) || s[e] == '_')) e++;
        char name[96];
        snprintf(name, sizeof name, "%.*s", (int)(e - i), s + i);
        lookup(m, name, " ", out);
        m->i = e;
        return;
    }
    if (i < m->n && s[i] == '{') {
        size_t e = i + 1;
        int depth = 1;
        for (; e < m->n && depth; e++) {
            if (s[e] == '{') depth++;
            else if (s[e] == '}') depth--;
            if (s[e] == '\n') m->line++;
        }
        /* body: s[i+1 .. e-1) */
        const char *b = s + i + 1, *end = s + e - 1;
        char sep[16] = " ";
        bool plain = true;
        if (b < end && *b == '(') {
            const char *q = b + 1;
            for (; q < end && *q != ')'; q++) {
                if (*q == 'j' && q + 1 < end) {
                    char d = q[1];
                    const char *z = memchr(q + 2, d, (size_t)(end - q - 2));
                    if (!z) { plain = false; break; }
                    snprintf(sep, sizeof sep, "%.*s", (int)(z - q - 2), q + 2);
                    q = z;
                } else if (*q != '@') plain = false;
            }
            b = q < end ? q + 1 : end;
        }
        const char *nm = b;
        while (b < end && (isalnum((unsigned char)*b) || *b == '_')) b++;
        if (plain && b == end && b > nm) {
            char name[96];
            snprintf(name, sizeof name, "%.*s", (int)(b - nm), nm);
            lookup(m, name, sep, out);
        } else sb_add(out, s + i - 1, e - i + 1);    /* not ours to evaluate: as written */
        m->i = e;
        return;
    }
    sb_chr(out, '$');                                 /* $1, $#, $(...), $+x: as written */
}

/* $'...' */
static void ansi_c(struct imp *m, struct sb *out) {
    struct sb raw = { 0 };
    for (; m->i < m->n && m->s[m->i] != '\''; m->i++) {
        if (m->s[m->i] == '\\' && m->i + 1 < m->n) {
            if (m->s[m->i + 1] == '\'') { sb_chr(&raw, '\''); m->i++; continue; }
            sb_chr(&raw, '\\');
            m->i++;
        }
        if (m->s[m->i] == '\n') m->line++;
        sb_chr(&raw, m->s[m->i]);
    }
    if (m->i >= m->n) fail(m, "unterminated $'...'");
    m->i++;
    p9k_decode_escapes(sb_text(&raw), out);
    sb_free(&raw);
}

/* One word: quotes removed, $ expanded, unquoted { , } as markers. `cond`:
 * inside [[ ]], where ( ) | & < > are part of words (; still ends one). */
static void word(struct imp *m, struct tok *t, bool cond) {
    const char *s = m->s;
    while (m->i < m->n) {
        char c = s[m->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == ';') return;
        if (!cond && (c == '&' || c == '|' || c == '(' || c == ')')) return;
        m->i++;
        if (c == '\\') {
            if (m->i < m->n && s[m->i] == '\n') { m->i++; m->line++; continue; }
            if (m->i < m->n) sb_chr(&t->text, s[m->i++]);
            t->quoted = true;
            continue;
        }
        if (c == '\'') {
            t->quoted = true;
            while (m->i < m->n && s[m->i] != '\'') { if (s[m->i] == '\n') m->line++; sb_chr(&t->text, s[m->i++]); }
            if (m->i >= m->n) { fail(m, "unterminated '...'"); return; }
            m->i++;
            continue;
        }
        if (c == '"') {
            t->quoted = true;
            while (m->i < m->n && s[m->i] != '"') {
                char d = s[m->i++];
                if (d == '\\' && m->i < m->n && strchr("\"\\$`\n", s[m->i])) {
                    if (s[m->i] != '\n') sb_chr(&t->text, s[m->i]);
                    else m->line++;
                    m->i++;
                } else if (d == '$') dollar(m, &t->text);
                else { if (d == '\n') m->line++; sb_chr(&t->text, d); }
            }
            if (m->i >= m->n) { fail(m, "unterminated \"...\""); return; }
            m->i++;
            continue;
        }
        if (c == '$' && m->i < m->n && s[m->i] == '\'') { m->i++; t->quoted = true; ansi_c(m, &t->text); continue; }
        if (c == '$') { dollar(m, &t->text); continue; }
        if (c == '{') { sb_chr(&t->text, BO); continue; }
        if (c == ',') { sb_chr(&t->text, CM); continue; }
        if (c == '}') { sb_chr(&t->text, BC); continue; }
        sb_chr(&t->text, c);
    }
}

/* Brace expansion: each result followed by WSEP, the markers left literal. */
static void braces(const char *w, struct sb *out) {
    for (size_t i = 0; w[i]; i++) {
        if (w[i] != BO) continue;
        int depth = 0;
        size_t j = i, commas[64], nc = 0;
        for (; w[j]; j++) {
            if (w[j] == BO) depth++;
            else if (w[j] == BC) { if (!--depth) break; }
            else if (w[j] == CM && depth == 1 && nc < 64) commas[nc++] = j;
        }
        if (!w[j] || !nc) continue;                   /* not a list */
        size_t from = i + 1;
        for (size_t k = 0; k <= nc; k++) {
            size_t to = k < nc ? commas[k] : j;
            struct sb t = { 0 };
            sb_add(&t, w, i);
            sb_add(&t, w + from, to - from);
            sb_str(&t, w + j + 1);
            braces(sb_text(&t), out);
            sb_free(&t);
            from = to + 1;
        }
        return;
    }
    for (const char *p = w; *p; p++) sb_chr(out, *p == BO ? '{' : *p == BC ? '}' : *p == CM ? ',' : *p);
    sb_chr(out, WSEP);
}

/* An assignment's name part: NAME= or NAME+= (braces allowed in NAME). */
static bool assignment_name(const char *w) {
    const char *eq = strchr(w, '=');
    if (!eq || eq == w) return false;
    for (const char *p = w; p < eq; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != BO && *p != BC && *p != CM && !(*p == '+' && p + 1 == eq)) return false;
    return !isdigit((unsigned char)w[0]);
}

static void skip_blank(struct imp *m) {
    for (;;) {
        while (m->i < m->n && (m->s[m->i] == ' ' || m->s[m->i] == '\t')) m->i++;
        if (m->i + 1 < m->n && m->s[m->i] == '\\' && m->s[m->i + 1] == '\n') { m->i += 2; m->line++; continue; }
        if (m->i < m->n && m->s[m->i] == '#') { while (m->i < m->n && m->s[m->i] != '\n') m->i++; }
        return;
    }
}

/* The next token. */
static void next(struct imp *m, struct tok *t) {
    tok_free(t);
    memset(t, 0, sizeof *t);
    skip_blank(m);
    if (m->i >= m->n) { t->kind = TK_EOF; return; }
    const char *s = m->s;
    char c = s[m->i];
    if (c == '\n') { m->i++; m->line++; t->kind = TK_SEP; return; }
    if (c == ';') { m->i++; t->kind = TK_SEP; if (m->i < m->n && s[m->i] == ';') m->i++; return; }
    if (c == '&') { m->i++; if (m->i < m->n && s[m->i] == '&') { m->i++; t->kind = TK_AND; } else t->kind = TK_SEP; return; }
    if (c == '|') { m->i++; if (m->i < m->n && s[m->i] == '|') { m->i++; t->kind = TK_OR; } else t->kind = TK_PIPE; return; }
    if (c == '(' && m->i + 1 < m->n && s[m->i + 1] == '(') {     /* (( arithmetic )) */
        size_t e = m->i + 2;
        int depth = 2;
        for (; e < m->n && depth; e++) {
            if (s[e] == '(') depth++;
            else if (s[e] == ')') depth--;
            else if (s[e] == '\n') m->line++;
        }
        t->kind = TK_ARITH;
        if (e - m->i >= 4) sb_add(&t->text, s + m->i + 2, e - m->i - 4);
        m->i = e;
        return;
    }
    if (c == '(') { m->i++; t->kind = TK_LPAREN; return; }
    if (c == ')') { m->i++; t->kind = TK_RPAREN; return; }
    t->kind = TK_WORD;
    word(m, t, false);
    if (t->text.n == 1 && (t->text.p[0] == BO || t->text.p[0] == BC)) t->text.p[0] = t->text.p[0] == BO ? '{' : '}';
    const char *w = sb_text(&t->text);
    if (!t->quoted && !strcmp(w, "[[")) {                      /* [[ ... ]]: its words */
        t->kind = TK_COND;
        for (;;) {
            while (m->i < m->n && strchr(" \t\n", s[m->i])) { if (s[m->i] == '\n') m->line++; m->i++; }
            if (m->i < m->n && s[m->i] == '#') { while (m->i < m->n && s[m->i] != '\n') m->i++; continue; }
            if (m->i >= m->n) { fail(m, "unterminated [["); return; }
            struct tok x = { 0 };
            word(m, &x, true);
            const char *xw = sb_text(&x.text);
            bool end = !x.quoted && !strcmp(xw, "]]");
            if (!end) {
                sb_chr(&t->elems, x.quoted ? 'q' : 'u');
                for (const char *p = xw; *p; p++) sb_chr(&t->elems, *p == BO ? '{' : *p == BC ? '}' : *p == CM ? ',' : *p);
                sb_chr(&t->elems, WSEP);
            }
            tok_free(&x);
            if (end || m->failed) return;
        }
    }
    if (m->i < m->n && s[m->i] == '(' && w[0] && w[strlen(w) - 1] == '=' && assignment_name(w)) {   /* NAME=( ... ) */
        t->kind = TK_ARRAY;
        m->i++;
        for (;;) {
            for (;;) {
                skip_blank(m);
                if (m->i < m->n && m->s[m->i] == '\n') { m->i++; m->line++; continue; }
                break;
            }
            if (m->i >= m->n) { fail(m, "unterminated ( in an array"); return; }
            if (s[m->i] == ')') { m->i++; return; }
            struct tok x = { 0 };
            word(m, &x, false);
            if (!x.text.n && !x.quoted) { tok_free(&x); fail(m, "unexpected character in an array"); return; }
            braces(sb_text(&x.text), &t->elems);
            tok_free(&x);
            if (m->failed) return;
        }
    }
}

/* --- commands ------------------------------------------------------------------ */

#define MAXT 64
struct cmd { struct tok t[MAXT]; int n; enum tk end; };

static void cmd_free(struct cmd *c) { for (int i = 0; i < c->n; i++) tok_free(&c->t[i]); c->n = 0; }

/* Tokens up to a separator; false at the end of the text. */
static bool read_cmd(struct imp *m, struct cmd *c) {
    cmd_free(c);
    struct tok t = { 0 };
    for (;;) {
        next(m, &t);
        if (m->failed) { tok_free(&t); return false; }
        if (t.kind == TK_SEP || t.kind == TK_AND || t.kind == TK_OR || t.kind == TK_PIPE || t.kind == TK_EOF) {
            c->end = t.kind;
            tok_free(&t);
            return c->n || c->end != TK_EOF;
        }
        if (c->n < MAXT) { c->t[c->n++] = t; memset(&t, 0, sizeof t); }
        else tok_free(&t);
    }
}

static bool is_word(const struct cmd *c, int i, const char *w) {
    return i < c->n && c->t[i].kind == TK_WORD && !c->t[i].quoted && !strcmp(sb_text(&c->t[i].text), w);
}

/* --- conditions ------------------------------------------------------------------ */

struct cw { const char *w[32]; bool q[32]; int n, at; };

static bool cond_expr(struct cw *c);
static bool cond_term(struct cw *c) {
    if (c->at >= c->n) return false;
    if (!c->q[c->at] && !strcmp(c->w[c->at], "!")) { c->at++; return !cond_term(c); }
    if (!c->q[c->at] && !strcmp(c->w[c->at], "(")) {
        c->at++;
        bool v = cond_expr(c);
        if (c->at < c->n && !strcmp(c->w[c->at], ")")) c->at++;
        return v;
    }
    const char *a = c->w[c->at];
    if (!c->q[c->at] && (!strcmp(a, "-n") || !strcmp(a, "-z")) && c->at + 1 < c->n) {
        bool empty = !c->w[c->at + 1][0];
        c->at += 2;
        return a[1] == 'z' ? empty : !empty;
    }
    if (c->at + 2 < c->n && !c->q[c->at + 1]) {
        const char *op = c->w[c->at + 1], *b = c->w[c->at + 2];
        bool bq = c->q[c->at + 2];
        if (!strcmp(op, "==") || !strcmp(op, "=") || !strcmp(op, "!=")) {
            c->at += 3;
            bool eq = bq ? !strcmp(a, b) : p9k_glob(b, strlen(b), a, strlen(a));
            return op[0] == '!' ? !eq : eq;
        }
    }
    c->at++;                                          /* -o opt, -f file...: not known here */
    return false;
}
static bool cond_expr(struct cw *c) {
    bool v = cond_term(c);
    while (c->at < c->n && !c->q[c->at] && (!strcmp(c->w[c->at], "&&") || !strcmp(c->w[c->at], "||"))) {
        bool and = c->w[c->at][0] == '&';
        c->at++;
        bool r = cond_term(c);
        v = and ? v && r : v || r;
    }
    return v;
}

static bool eval_cond(const struct tok *t) {
    if (t->kind != TK_COND) return false;            /* (( )) and commands: not evaluated */
    struct cw c = { 0 };
    char *buf = t->elems.p;
    if (!buf) return false;
    for (char *p = buf; *p && c.n < 32;) {
        char *e = strchr(p, WSEP);
        if (!e) break;
        *e = 0;
        c.q[c.n] = *p == 'q';
        c.w[c.n++] = p + 1;
        p = e + 1;
    }
    bool v = cond_expr(&c);
    for (char *p = buf; p < buf + t->elems.n; p++) if (!*p) *p = WSEP;
    return v;
}

/* --- blocks ------------------------------------------------------------------ */

enum fk { F_BLOCK, F_IF, F_FUNC };
struct frame {
    enum fk kind;
    bool run;                    /* its body runs */
    bool taken;                  /* F_IF: a branch ran */
    bool parent;                 /* F_IF: the code around it runs */
    bool formatter;              /* F_FUNC: my_git_formatter */
    bool dollar1;                /* F_IF: `if (( $1 ))`, the formatter's ready / loading switch */
    bool in_else;
};
#define MAXF 32

struct run { struct frame f[MAXF]; int n; };

static bool running(const struct run *r) { return !r->n || r->f[r->n - 1].run; }

/* Inside my_git_formatter: which colours a `local` sets -- the ready ones,
 * the loading ones (else of `if (( $1 ))`), or none (another branch). */
static int formatter_branch(const struct run *r) {
    int fn = -1;
    for (int i = r->n - 1; i >= 0; i--) if (r->f[i].kind == F_FUNC) { fn = i; break; }
    if (fn < 0 || !r->f[fn].formatter) return -1;
    if (fn == r->n - 1) return 0;
    if (fn == r->n - 2 && r->f[r->n - 1].dollar1) return r->f[r->n - 1].in_else ? 1 : 0;
    return -1;
}

static bool push(struct imp *m, struct run *r, struct frame f) {
    if (r->n == MAXF) { fail(m, "blocks nested too deep"); return false; }
    r->f[r->n++] = f;
    return true;
}

/* --- assignments ------------------------------------------------------------ */

static void assign(struct imp *m, bool local, const char *name, bool append, const char *value) {
    struct p10k *dst = m->vars;
    const char *key = name;
    if (!local && !strncmp(name, "POWERLEVEL9K_", 13)) { dst = m->cfg; key = name + 13; }
    else if (!local && !strcmp(name, "DEFAULT_USER")) { dst = m->cfg; key = "_DEFAULT_USER"; }
    if (!strcmp(key, "CONFIG_FILE") && dst == m->cfg) return;   /* where the file is: no use here */
    struct sb v = { 0 };
    const char *old = append ? p10k_get(dst, key) : NULL;
    if (old && old[0]) { sb_str(&v, old); if (value[0]) sb_chr(&v, P10K_ARRAY_SEP); }
    sb_str(&v, value);
    if (!p10k_set(dst, key, sb_text(&v)) || v.oom) fail(m, "out of memory");
    else if (dst == m->cfg && m->rep) m->rep->set++;
    sb_free(&v);
}

/* typeset / local / a bare NAME=value: every assignment in the command from `from`. */
static void declare(struct imp *m, struct cmd *c, int from, bool local) {
    for (int i = from; i < c->n; i++) {
        struct tok *t = &c->t[i];
        if (t->kind == TK_WORD && !t->quoted && sb_text(&t->text)[0] == '-') continue;    /* -g -a -i */
        if (t->kind == TK_WORD && !t->quoted && sb_text(&t->text)[0] == '+') continue;
        if (t->kind != TK_WORD && t->kind != TK_ARRAY) continue;
        const char *w = sb_text(&t->text);
        const char *eq = strchr(w, '=');
        /* the name part, brace-expanded; the value once */
        struct sb names = { 0 }, nm = { 0 };
        if (eq) sb_add(&nm, w, (size_t)(eq - w)); else sb_str(&nm, w);
        bool append = nm.n && nm.p[nm.n - 1] == '+';
        if (append) nm.p[--nm.n] = 0;
        braces(sb_text(&nm), &names);
        struct sb value = { 0 };
        if (t->kind == TK_ARRAY) {
            for (const char *p = sb_text(&t->elems); *p; p++) sb_chr(&value, *p == WSEP ? P10K_ARRAY_SEP : *p);
            if (value.n) value.p[--value.n] = 0;      /* the last separator */
        } else if (eq) {
            for (const char *p = eq + 1; *p; p++) sb_chr(&value, *p == BO ? '{' : *p == BC ? '}' : *p == CM ? ',' : *p);
        }
        for (char *p = names.p; p && *p;) {
            char *e = strchr(p, WSEP);
            if (!e) break;
            *e = 0;
            bool valid = (isalpha((unsigned char)*p) || *p == '_');
            for (const char *q = p; *q; q++) valid &= isalnum((unsigned char)*q) || *q == '_';
            if (valid) {
                if (eq || t->kind == TK_ARRAY) assign(m, local, p, append, sb_text(&value));
                else if (!p10k_get(local ? m->vars : m->cfg, !local && !strncmp(p, "POWERLEVEL9K_", 13) ? p + 13 : p))
                    assign(m, local, p, false, "");   /* declared: set, empty */
            }
            p = e + 1;
        }
        sb_free(&names); sb_free(&nm); sb_free(&value);
    }
}

/* `local meta='%f'` in my_git_formatter: its colour, for the engine's own. */
static void formatter_local(struct imp *m, struct cmd *c, int from, bool loading) {
    static const char *const parts[] = { "meta", "clean", "modified", "untracked", "conflicted" };
    for (int i = from; i < c->n; i++) {
        if (c->t[i].kind != TK_WORD) continue;
        const char *w = sb_text(&c->t[i].text), *eq = strchr(w, '=');
        if (!eq) continue;
        for (size_t k = 0; k < sizeof parts / sizeof parts[0]; k++) {
            if ((size_t)(eq - w) != strlen(parts[k]) || strncmp(w, parts[k], strlen(parts[k]))) continue;
            char key[48], up[16];
            size_t j = 0;
            for (; parts[k][j] && j + 1 < sizeof up; j++) up[j] = (char)toupper((unsigned char)parts[k][j]);
            up[j] = 0;
            snprintf(key, sizeof key, "_GIT_FORMAT_%s%s", loading ? "LOADING_" : "", up);
            if (!p10k_set(m->cfg, key, eq + 1)) fail(m, "out of memory");
        }
    }
}

/* --- the file ------------------------------------------------------------------ */

static void report_segments(struct imp *m) {
    if (!m->rep) return;
    char *out = m->rep->ignored;
    size_t cap = sizeof m->rep->ignored;
    out[0] = 0;
    const char *lists[] = { p10k_get(m->cfg, "LEFT_PROMPT_ELEMENTS"), p10k_get(m->cfg, "RIGHT_PROMPT_ELEMENTS") };
    for (int l = 0; l < 2; l++) {
        char e[64];
        size_t n = p9k_array(lists[l], 0, e, sizeof e);
        for (size_t i = 0; i < n; i++) {
            p9k_array(lists[l], i, e, sizeof e);
            size_t len = strlen(e);
            if (len > 7 && !strcmp(e + len - 7, "_joined")) e[len - 7] = 0;
            if (!e[0] || p10k_segment_drawn(e)) continue;
            /* once each */
            bool seen = false;
            for (const char *p = out; (p = strstr(p, e)); p += strlen(e))
                if ((p == out || p[-1] == ' ') && (!p[strlen(e)] || p[strlen(e)] == ' ')) { seen = true; break; }
            if (seen) continue;
            size_t at = strlen(out);
            if (at + strlen(e) + 2 < cap) snprintf(out + at, cap - at, "%s%s", at ? " " : "", e);
        }
    }
}

bool p10k_import(struct p10k *cfg, const char *text, struct p10k_import *report) {
    if (report) memset(report, 0, sizeof *report);
    struct imp m = { cfg, p10k_new(), text, 0, strlen(text), 1, report, false };
    if (!m.vars) { if (report) snprintf(report->error, sizeof report->error, "out of memory"); return false; }
    struct run r = { 0 };
    struct cmd *c = calloc(1, sizeof *c);
    if (!c) { p10k_free(m.vars); if (report) snprintf(report->error, sizeof report->error, "out of memory"); return false; }
    bool skip_list = false;                           /* inside a list joined by && or || */
    while (!m.failed && read_cmd(&m, c)) {
        bool joined = c->end == TK_AND || c->end == TK_OR || c->end == TK_PIPE;
        if (skip_list || (joined && !is_word(c, 0, "if") && !is_word(c, 0, "elif"))) {
            skip_list = joined;
            continue;
        }
        if (!c->n) continue;
        int at = 0;
        /* then / else / do lead the next command on their line */
        for (;;) {
            if (is_word(c, at, "then") || is_word(c, at, "do")) { at++; continue; }
            if (is_word(c, at, "else")) {
                struct frame *f = r.n ? &r.f[r.n - 1] : NULL;
                if (!f || f->kind != F_IF) { fail(&m, "else without if"); break; }
                f->run = f->parent && !f->taken;
                f->taken = true;
                f->in_else = true;
                at++;
                continue;
            }
            break;
        }
        if (m.failed) break;
        if (at >= c->n) continue;
        if (is_word(c, at, "if") || is_word(c, at, "elif")) {
            bool elif = is_word(c, at, "elif");
            /* the condition: this command and those joined to it by && and || */
            bool v = c->n > at + 1 && eval_cond(&c->t[at + 1]);
            bool dollar1 = false;
            if (c->n > at + 1 && c->t[at + 1].kind == TK_ARITH) {
                const char *a = sb_text(&c->t[at + 1].text);
                while (*a == ' ') a++;
                dollar1 = !strncmp(a, "$1", 2) && !a[2 + strspn(a + 2, " ")];
            }
            enum tk op = c->end;
            while (op == TK_AND || op == TK_OR) {
                if (!read_cmd(&m, c)) break;
                bool w = c->n && eval_cond(&c->t[0]);
                v = op == TK_AND ? v && w : v || w;
                dollar1 = false;
                op = c->end;
            }
            if (elif) {
                struct frame *f = r.n ? &r.f[r.n - 1] : NULL;
                if (!f || f->kind != F_IF) { fail(&m, "elif without if"); break; }
                f->run = f->parent && !f->taken && v;
                f->taken |= v;
                f->dollar1 = false;
            } else {
                bool parent = running(&r);
                push(&m, &r, (struct frame){ F_IF, parent && v, v, parent, false, dollar1, false });
            }
            /* `if ...; then cmd` on one line: then's command */
            continue;
        }
        if (is_word(c, at, "fi")) {
            if (!r.n || r.f[r.n - 1].kind != F_IF) { fail(&m, "fi without if"); break; }
            r.n--;
            continue;
        }
        if (is_word(c, at, "}")) {
            if (!r.n || r.f[r.n - 1].kind == F_IF) { fail(&m, "} without {"); break; }
            r.n--;
            continue;
        }
        /* functions: `function NAME() {`, `NAME() {`; `() {` and `{` run */
        int k = at;
        bool fn = is_word(c, k, "function");
        if (fn) k++;
        const char *name = NULL;
        if (k < c->n && c->t[k].kind == TK_WORD && !is_word(c, k, "{") && (fn || (k + 1 < c->n && c->t[k + 1].kind == TK_LPAREN))) {
            name = sb_text(&c->t[k].text);
            k++;
        }
        if (k + 1 < c->n && c->t[k].kind == TK_LPAREN && c->t[k + 1].kind == TK_RPAREN) k += 2;
        if ((fn || name || k > at) && is_word(c, k, "{")) {
            if (name) push(&m, &r, (struct frame){ F_FUNC, false, false, false, !strcmp(name, "my_git_formatter"), false, false });
            else push(&m, &r, (struct frame){ F_BLOCK, running(&r), false, false, false, false, false });
            if (k + 1 < c->n) fail(&m, "a command after { on its line");
            continue;
        }
        if (is_word(c, at, "{")) { push(&m, &r, (struct frame){ F_BLOCK, running(&r), false, false, false, false, false }); continue; }
        if (fn) { fail(&m, "a function without { on its line"); break; }

        /* assignments */
        int from = at;
        if (is_word(c, from, "builtin")) from++;
        bool local = is_word(c, from, "local");
        int branch = formatter_branch(&r);
        if (branch >= 0) {
            if (local) formatter_local(&m, c, from + 1, branch == 1);
            continue;
        }
        if (!running(&r)) continue;
        if (local || is_word(c, from, "typeset") || is_word(c, from, "declare") || is_word(c, from, "export")
            || is_word(c, from, "readonly") || is_word(c, from, "integer") || is_word(c, from, "float")) {
            bool global = false;
            for (int i = from + 1; i < c->n; i++)
                if (c->t[i].kind == TK_WORD && sb_text(&c->t[i].text)[0] == '-' && strchr(sb_text(&c->t[i].text), 'g')) global = true;
            /* typeset without -g inside a function is local to it; the
             * configuration's anonymous function counts as the file */
            declare(&m, c, from + 1, local && !global);
            continue;
        }
        if (is_word(c, from, "unset")) {
            for (int i = from + 1; i < c->n; i++) {
                if (c->t[i].kind != TK_WORD) continue;
                const char *w = sb_text(&c->t[i].text);
                if (w[0] == '-') continue;
                if (strstr(w, "POWERLEVEL9K_*")) {           /* unset -m '(POWERLEVEL9K_*|...)' */
                    p10k_clear(cfg);
                    if (report) report->set = 0;
                    continue;
                }
                if (!strncmp(w, "POWERLEVEL9K_", 13)) p10k_unset(cfg, w + 13);
                else p10k_unset(m.vars, w);
            }
            continue;
        }
        /* NAME=value alone: an assignment; before a command: that command's */
        bool only = true;
        for (int i = from; i < c->n; i++)
            if (!((c->t[i].kind == TK_WORD && assignment_name(sb_text(&c->t[i].text))) || c->t[i].kind == TK_ARRAY)) only = false;
        if (only) declare(&m, c, from, false);
    }
    if (!m.failed && r.n) fail(&m, r.f[r.n - 1].kind == F_IF ? "if without fi" : "{ without }");
    report_segments(&m);
    cmd_free(c);
    free(c);
    p10k_free(m.vars);
    return !m.failed;
}
