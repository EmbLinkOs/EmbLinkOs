/* ==========================================================================
 * highlight.c -- see highlight.h.
 *
 * The line is lexed as the shell will lex it, and walked token by token with
 * where each stands: a COMMAND's place (the start of a statement, after |, ;,
 * &, {, `$(`), an ARGUMENT's, an EXPRESSION's (after if, while, in, return,
 * `=`), or a NAME's (after for, let, def). Comments are what the lexer skips
 * between tokens, from a '#'. A line that does not lex -- a quote not closed
 * yet -- is drawn as far as it goes: the open quote's text yellow to the end,
 * as zsh-syntax-highlighting draws one.
 *
 * Files are looked for in directory listings, never by opening a name that
 * may not be there (a missing file costs a line in the kernel's log, and the
 * line is drawn on every key). The programs are listed once per prompt.
 * ========================================================================== */
#include "complete/highlight.h"
#include "edit/edit.h"
#include "lex/lex.h"
#include "builtins/builtins.h"
#include "eval/eval.h"
#include "eval/exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_LEN 512

/* --- the names in a directory ------------------------------------------------ */

struct names { char **v; size_t n, cap; bool listed, ok; };

static void names_free(struct names *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* Each name with its kind before it: 'd' a directory, 'f' anything else. */
static void names_add(void *ctx, const char *name, bool dir) {
    struct names *l = ctx;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 32;
        char **v = realloc(l->v, cap * sizeof *v);
        if (!v) return;
        l->v = v;
        l->cap = cap;
    }
    size_t len = strlen(name);
    char *s = malloc(len + 2);
    if (!s) return;
    s[0] = dir ? 'd' : 'f';
    memcpy(s + 1, name, len + 1);
    l->v[l->n++] = s;
}

static void names_list(struct names *l, const char *dir) {
    if (l->listed) return;
    l->listed = true;
    l->ok = shell_list_dir(dir, names_add, l) >= 0;
}

/* 'd', 'f', or 0 when `dir`'s listing does not hold `name`. */
static char kind_of(const struct names *l, const char *name) {
    for (size_t i = 0; i < l->n; i++) if (!strcmp(l->v[i] + 1, name)) return l->v[i][0];
    return 0;
}

/* --- the programs, per prompt ------------------------------------------------ */

static struct names g_bin, g_apps;

void shell_highlight_fresh(void) {
    names_free(&g_bin);
    names_free(&g_apps);
}

/* shell_resolve_program's search: /system/bin/NAME.elf, /system/bin/NAME,
 * /data/apps/NAME/NAME.elf. */
static bool program(const char *name) {
    char file[PATH_LEN];
    names_list(&g_bin, "/system/bin");
    names_list(&g_apps, "/data/apps");
    snprintf(file, sizeof file, "%s.elf", name);
    if (kind_of(&g_bin, file) == 'f' || kind_of(&g_bin, name) == 'f') return true;
    if (kind_of(&g_apps, name) != 'd') return false;
    char dir[PATH_LEN];
    snprintf(dir, sizeof dir, "/data/apps/%s", name);
    struct names app = { 0 };
    names_list(&app, dir);
    bool found = kind_of(&app, file) == 'f';
    names_free(&app);
    return found;
}

static bool command(const char *name) {
    if (!strcmp(name, "exit")) return true;             /* the shell's own, read before the language */
    if (builtin_lookup(name) || builtin_lookup_os(name)) return true;
    for (size_t i = 0; i < exec_user_command_count(); i++) if (!strcmp(exec_user_command_name(i), name)) return true;
    return program(name);
}

/* --- files the line names ---------------------------------------------------- */

#define DIRS 8
struct look { char dir[DIRS][PATH_LEN]; struct names names[DIRS]; int n; };

static const struct names *listing(struct look *k, const char *dir) {
    for (int i = 0; i < k->n; i++) if (!strcmp(k->dir[i], dir)) return &k->names[i];
    int i = k->n < DIRS ? k->n++ : DIRS - 1;
    names_free(&k->names[i]);
    snprintf(k->dir[i], PATH_LEN, "%s", dir);
    names_list(&k->names[i], dir);
    return &k->names[i];
}

/* `word` as a path from where the shell is: its directory and its last name
 * ("" after a trailing slash). False when it is too long. */
static bool split(const char *word, char *dir, char *base) {
    char full[PATH_LEN * 2];
    if (word[0] == '/') snprintf(full, sizeof full, "%s", word);
    else {
        char cwd[PATH_LEN];
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
        snprintf(full, sizeof full, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", word);
    }
    if (strlen(full) >= PATH_LEN) return false;
    char *slash = strrchr(full, '/');
    snprintf(base, PATH_LEN, "%s", slash + 1);
    slash[slash == full ? 1 : 0] = '\0';
    snprintf(dir, PATH_LEN, "%s", full);
    return true;
}

/* 'd' or 'f' when `word` names something that is there. */
static char exists(struct look *k, const char *word) {
    char dir[PATH_LEN], base[PATH_LEN], trimmed[PATH_LEN];
    snprintf(trimmed, sizeof trimmed, "%s", word);
    size_t len = strlen(trimmed);
    while (len > 1 && trimmed[len - 1] == '/') trimmed[--len] = '\0';
    if (!strcmp(trimmed, "/")) return 'd';
    if (!split(trimmed, dir, base)) return 0;
    if (!strcmp(base, ".") || !strcmp(base, "..")) return 'd';
    char kind = kind_of(listing(k, dir), base);
    return len < strlen(word) && kind != 'd' ? 0 : kind;   /* "file/" names no directory */
}

/* Whether something in `word`'s directory begins with its last name: a word
 * on its way to a file's name (zsh-syntax-highlighting's path_prefix). */
static bool prefix(struct look *k, const char *word) {
    char dir[PATH_LEN], base[PATH_LEN];
    if (!split(word, dir, base)) return false;
    const struct names *l = listing(k, dir);
    size_t bl = strlen(base);
    for (size_t i = 0; i < l->n; i++) {
        const char *name = l->v[i] + 1;
        if (name[0] == '.' && base[0] != '.') continue;   /* a glob does not find them either */
        if (!strncmp(name, base, bl)) return true;
    }
    return false;
}

/* --- the line ---------------------------------------------------------------- */

static void fill(unsigned char *style, size_t from, size_t n, unsigned char s) {
    memset(style + from, s, n);
}

/* Quoted text: yellow, a double quote's escapes cyan. */
static void quoted(const char *src, size_t off, size_t end, unsigned char *style) {
    bool dq = src[off] == '"';
    for (size_t i = off; i < end; i++) {
        if (dq && i > off && src[i] == '\\' && i + 1 < end) {
            style[i] = style[i + 1] = EDIT_HL_ESCAPE;
            i++;
            continue;
        }
        style[i] = EDIT_HL_QUOTED;
    }
}

/* Comments in what the lexer skipped between `from` and `to`. */
static void comments(const char *src, size_t from, size_t to, unsigned char *style) {
    for (size_t i = from; i < to; i++) {
        if (src[i] != '#') continue;
        size_t e = i;
        while (e < to && src[e] != '\n') e++;
        fill(style, i, e - i, EDIT_HL_COMMENT);
        i = e;
    }
}

enum where { COMMAND, ARGUMENT, EXPRESSION, NAME };

void shell_highlight(void *ctx, const char *text, size_t n, bool done, unsigned char *style) {
    (void)ctx;
    char *src = malloc(n + 1);
    if (!src) return;
    memcpy(src, text, n);
    src[n] = '\0';
    size_t ntok = 0;
    struct token *t = lex(src, &ntok);
    if (!t) { free(src); return; }
    struct look k;
    k.n = 0;
    memset(k.names, 0, sizeof k.names);
    enum where at = COMMAND;
    char open[64];                       /* what each ( { $( that is open opened */
    int depth = 0;
    bool target = false;                 /* the word after a redirection names a file */
    size_t gap = 0;                      /* where the text the lexer skipped begins */
    for (size_t i = 0; i < ntok && t[i].type != TOK_EOF; i++) {
        const struct token *tk = &t[i];
        if (tk->type == TOK_ERROR) {
            /* a quote not closed yet: its text to the end; another error: the
             * character it stopped at */
            size_t q = gap;
            while (q < n && (src[q] == ' ' || src[q] == '\t')) q++;
            comments(src, gap, q, style);
            if (q < n && (src[q] == '"' || src[q] == '\'')) quoted(src, q, n, style);
            else if (q < n) style[q] = EDIT_HL_UNKNOWN;
            gap = n;
            break;
        }
        size_t off = (size_t)(tk->lexeme - src), len = tk->lexeme_len;
        if (off > n || off + len > n) break;
        comments(src, gap, off, style);
        gap = off + len;
        bool last = off + len == n && !done;
        char word[PATH_LEN];
        snprintf(word, sizeof word, "%.*s", (int)(len < PATH_LEN ? len : PATH_LEN - 1), tk->lexeme);
        bool was_target = target;
        target = false;
        switch (tk->type) {
        case TOK_NEWLINE: case TOK_SEMI: case TOK_AMP: case TOK_PIPE:
            at = COMMAND;
            break;
        case TOK_LBRACE:
            fill(style, off, len, EDIT_HL_RESERVED);
            if (depth < (int)sizeof open) open[depth++] = '{';
            at = COMMAND;
            break;
        case TOK_RBRACE:
            if (depth && open[depth - 1] == '{') { depth--; fill(style, off, len, EDIT_HL_RESERVED); }
            else fill(style, off, len, EDIT_HL_UNKNOWN);
            at = COMMAND;
            break;
        case TOK_DOLLAR_LPAREN:
            fill(style, off, len, EDIT_HL_SUBSTITUTION);
            if (depth < (int)sizeof open) open[depth++] = '$';
            at = COMMAND;
            break;
        case TOK_LPAREN:
            if (depth < (int)sizeof open) open[depth++] = '(';
            at = EXPRESSION;
            break;
        case TOK_RPAREN:
            if (depth && open[depth - 1] == '$') { depth--; fill(style, off, len, EDIT_HL_SUBSTITUTION); }
            else if (depth && open[depth - 1] == '(') depth--;
            else fill(style, off, len, EDIT_HL_UNKNOWN);
            at = ARGUMENT;
            break;
        case TOK_IF: case TOK_WHILE: case TOK_RETURN:
            fill(style, off, len, EDIT_HL_RESERVED);
            at = EXPRESSION;
            break;
        case TOK_FOR: case TOK_LET: case TOK_DEF:
            fill(style, off, len, EDIT_HL_RESERVED);
            at = NAME;
            break;
        case TOK_ELSE:
            fill(style, off, len, EDIT_HL_RESERVED);
            at = COMMAND;
            break;
        case TOK_BREAK: case TOK_CONTINUE:
            fill(style, off, len, EDIT_HL_RESERVED);
            at = ARGUMENT;
            break;
        case TOK_IN: case TOK_ASSIGN:                     /* `in` is not a reserved word to it either */
            at = EXPRESSION;
            break;
        case TOK_REDIRECT:
            fill(style, off, len, EDIT_HL_REDIRECTION);
            target = true;
            break;
        case TOK_STRING:
            quoted(src, off, off + len, style);
            if (at == COMMAND) at = ARGUMENT;
            break;
        case TOK_IDENT:
            if (was_target || at == ARGUMENT) {
                if (word[0] != '-' && (exists(&k, word) || (last && prefix(&k, word))))
                    fill(style, off, len, EDIT_HL_PATH);
            } else if (at == COMMAND) {
                if (strchr(word, '/')) {
                    char kind = exists(&k, word);
                    if (kind == 'f') fill(style, off, len, EDIT_HL_COMMAND);
                    else fill(style, off, len, last && prefix(&k, word) ? EDIT_HL_PATH : EDIT_HL_UNKNOWN);
                } else fill(style, off, len, command(word) ? EDIT_HL_COMMAND : EDIT_HL_UNKNOWN);
                at = ARGUMENT;
            } else if (at == NAME) at = EXPRESSION;
            break;
        default:                                          /* a value: an expression's stage */
            if (at == COMMAND) at = ARGUMENT;
            break;
        }
    }
    if (gap < n) comments(src, gap, n, style);
    for (int i = 0; i < k.n; i++) names_free(&k.names[i]);
    lex_free_tokens(t, ntok);
    free(src);
}
