/* ==========================================================================
 * complete.c -- see complete.h.
 *
 * The word being completed runs back from the cursor to a space or to
 * whatever ends a command (| ; ( ) { } &), or to an open double quote: inside
 * one, the rest of the line up to the cursor is the word, so a quoted path
 * with a space in it completes like any other.
 *
 * A word is in COMMAND position when nothing but spaces stands between it and
 * the start of a statement (the line's start, a newline, '|', ';', '&', '('
 * or '{'). There it completes as a command; with a '/' in it, as a path.
 * ========================================================================== */
#include "complete/complete.h"
#include "builtins/builtins.h"
#include "eval/eval.h"
#include "eval/exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_LEN 512

static bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/';
}

/* Whether the lexer reads `s` as ONE bare word (lex.c: word characters, with
 * '-' and '.' only between two of them, not starting with a digit or a dot).
 * Anything else is offered in double quotes, which always reads as text. */
static bool bare(const char *s) {
    while (!strncmp(s, "./", 2) || !strncmp(s, "../", 3)) s += s[1] == '/' ? 2 : 3;   /* lex.c's dot paths */
    if (!*s) return true;
    if ((*s >= '0' && *s <= '9') || *s == '.' || *s == '-') return false;
    for (const char *p = s; *p; p++) {
        if (word_char(*p)) continue;
        if ((*p == '-' || *p == '.') && p > s && word_char(p[-1]) && word_char(p[1])) continue;
        return false;
    }
    return true;
}

/* `"` + s with `"` and `\` escaped; the closing quote only when asked. */
static void quote(char *out, size_t cap, const char *s, bool close) {
    size_t n = 0;
    if (n + 1 < cap) out[n++] = '"';
    for (const char *p = s; *p && n + 3 < cap; p++) {
        if (*p == '"' || *p == '\\') out[n++] = '\\';
        out[n++] = *p;
    }
    if (close && n + 1 < cap) out[n++] = '"';
    out[n] = '\0';
}

/* --- candidates ----------------------------------------------------------- */

static void offer_name(struct edit_matches *m, const char *name, const char *prefix) {
    if (strncmp(name, prefix, strlen(prefix)) != 0) return;
    for (size_t i = 0; i < m->n; i++) if (!strcmp(m->items[i].insert, name)) return;
    edit_matches_add(m, name, name, false);
}

struct listing {
    struct edit_matches *m;
    const char *typed_dir;     /* the directory part as typed, kept in what is inserted */
    const char *base;          /* what the name must start with */
    bool quoted;               /* the word is inside an open double quote */
};

static void offer_file(void *ctx, const char *name, bool dir) {
    struct listing *l = ctx;
    if (strncmp(name, l->base, strlen(l->base)) != 0) return;
    if (name[0] == '.' && l->base[0] != '.') return;            /* hidden unless asked for */
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    char path[PATH_LEN], insert[PATH_LEN * 2 + 4], show[PATH_LEN];
    snprintf(path, sizeof path, "%s%s%s", l->typed_dir, name, dir ? "/" : "");
    snprintf(show, sizeof show, "%s%s", name, dir ? "/" : "");
    if (l->quoted || !bare(path))
        quote(insert, sizeof insert, path, !dir);     /* a directory stays open to go on into */
    else
        snprintf(insert, sizeof insert, "%s", path);
    edit_matches_add(l->m, insert, show, dir);
}

static void complete_path(struct edit_matches *m, const char *word, bool quoted) {
    const char *slash = strrchr(word, '/');
    char typed_dir[PATH_LEN] = "", where[PATH_LEN];
    const char *base = word;
    if (slash) {
        size_t n = (size_t)(slash - word) + 1;
        if (n >= sizeof typed_dir) return;
        memcpy(typed_dir, word, n);
        typed_dir[n] = '\0';
        base = slash + 1;
    }
    if (typed_dir[0] == '/') snprintf(where, sizeof where, "%s", typed_dir);
    else {
        char cwd[PATH_LEN];
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "/");
        snprintf(where, sizeof where, "%s/%s", cwd, typed_dir);
    }
    struct listing l = { m, typed_dir, base, quoted };
    shell_list_dir(where, offer_file, &l);
}

/* Programs the shell would run by bare name (eval_extern.c's search):
 * /system/bin/<name>[.elf] and /data/apps/<name>/<name>.elf. */
struct programs { struct edit_matches *m; const char *prefix; bool apps; };
static void offer_program(void *ctx, const char *name, bool dir) {
    struct programs *p = ctx;
    char bare_name[PATH_LEN];
    snprintf(bare_name, sizeof bare_name, "%s", name);
    if (p->apps) {
        if (!dir || name[0] == '.') return;
    } else {
        if (dir) return;
        size_t n = strlen(bare_name);
        if (n > 4 && !strcmp(bare_name + n - 4, ".elf")) bare_name[n - 4] = '\0';
    }
    offer_name(p->m, bare_name, p->prefix);
}

static void complete_command(struct edit_matches *m, const char *word) {
    static const char *const keywords[] = {
        "if", "else", "while", "for", "def", "let", "return", "break", "continue", "exit",
    };
    for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++) offer_name(m, keywords[i], word);
    for (size_t i = 0; builtin_name(i); i++) offer_name(m, builtin_name(i), word);
    for (size_t i = 0; i < exec_user_command_count(); i++) offer_name(m, exec_user_command_name(i), word);
    struct programs bin = { m, word, false }, apps = { m, word, true };
    shell_list_dir("/system/bin", offer_program, &bin);
    shell_list_dir("/data/apps", offer_program, &apps);
}

static void complete_variable(struct edit_matches *m, const struct scope *top, const char *word) {
    const char *prefix = word + 1;
    char insert[160];
    for (const struct scope *s = top; s; s = s->parent)
        for (size_t i = 0; i < s->count; i++) {
            if (strncmp(s->names[i], prefix, strlen(prefix)) != 0) continue;
            snprintf(insert, sizeof insert, "$%s", s->names[i]);
            bool seen = false;
            for (size_t j = 0; j < m->n; j++) seen |= !strcmp(m->items[j].insert, insert);
            if (!seen) edit_matches_add(m, insert, insert, false);
        }
}

static int by_name(const void *a, const void *b) {
    const struct edit_item *x = a, *y = b;
    return strcmp(x->show, y->show);
}

void shell_complete(void *ctx, const char *line, size_t cur, struct edit_matches *m) {
    /* Inside an open double quote? Then the word starts at that quote. */
    bool inq = false, insq = false;
    size_t qstart = 0;
    for (size_t i = 0; i < cur; i++) {
        if (inq && line[i] == '\\') { i++; continue; }
        if (!insq && line[i] == '"') { inq = !inq; qstart = i; }
        else if (!inq && line[i] == '\'') { insq = !insq; qstart = i; }
    }
    if (insq) return;                              /* single quotes: literal text, nothing to offer */
    size_t start = cur;
    char word[PATH_LEN];
    if (inq) {
        start = qstart;
        size_t n = 0;
        for (size_t i = qstart + 1; i < cur && n + 1 < sizeof word; i++) {
            if (line[i] == '\\' && i + 1 < cur) i++;
            word[n++] = line[i];
        }
        word[n] = '\0';
    } else {
        while (start > 0 && !strchr(" \t\n|;(){}&", line[start - 1])) start--;
        size_t n = cur - start < sizeof word - 1 ? cur - start : sizeof word - 1;
        memcpy(word, line + start, n);
        word[n] = '\0';
    }
    m->start = start;

    size_t p = start;
    while (p > 0 && (line[p - 1] == ' ' || line[p - 1] == '\t')) p--;
    bool command = p == 0 || strchr("\n|;&({", line[p - 1]) != NULL;

    if (!inq && word[0] == '$') complete_variable(m, ctx, word);
    else if (command && !inq && !strchr(word, '/') && !(word[0] == '.' && word[1] == '/')) complete_command(m, word);
    else complete_path(m, word, inq);

    if (m->n > 1) qsort(m->items, m->n, sizeof *m->items, by_name);
}
