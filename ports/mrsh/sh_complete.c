/* sh_complete.c -- see sh_complete.h.
 *
 * The word runs back from the cursor to unescaped whitespace or one of
 * ; & | ( ) < >, or to an open quote. It is in command position when only
 * whitespace stands between it and the start of a command. */
#define _POSIX_C_SOURCE 200809L
#include "sh_complete.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mrsh/shell.h>

#define PATH_LEN 512

/* POSIX shell specials: anything here is backslash-escaped when inserted. */
static bool special(char c) { return strchr(" \t\n\\'\"`$&|;()<>*?[]#~=%!{}", c) != NULL; }

static void escaped(char *out, size_t cap, const char *s) {
    size_t n = 0;
    for (const char *p = s; *p && n + 3 < cap; p++) {
        if (special(*p)) out[n++] = '\\';
        out[n++] = *p;
    }
    out[n] = '\0';
}

static void offer(struct edit_matches *m, const char *insert, const char *show, bool dir) {
    for (size_t i = 0; i < m->n; i++) if (!strcmp(m->items[i].insert, insert)) return;
    edit_matches_add(m, insert, show, dir);
}

static void each_entry(const char *dir, void (*fn)(void *, const char *, bool), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *de; (de = readdir(d));) {
        char full[PATH_LEN * 2];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        fn(ctx, de->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
}

struct files { struct edit_matches *m; const char *typed_dir, *base; bool quoted; char quote; };
static void offer_file(void *ctx, const char *name, bool dir) {
    struct files *f = ctx;
    if (strncmp(name, f->base, strlen(f->base)) != 0) return;
    if (name[0] == '.' && f->base[0] != '.') return;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;
    char path[PATH_LEN], insert[PATH_LEN * 2 + 4], show[PATH_LEN];
    snprintf(path, sizeof path, "%s%s%s", f->typed_dir, name, dir ? "/" : "");
    snprintf(show, sizeof show, "%s%s", name, dir ? "/" : "");
    if (f->quoted) snprintf(insert, sizeof insert, "%c%s%s", f->quote, path, dir ? "" : (char[]){ f->quote, 0 });
    else escaped(insert, sizeof insert, path);
    offer(f->m, insert, show, dir);
}

struct programs { struct edit_matches *m; const char *prefix; bool apps; };
static void offer_program(void *ctx, const char *name, bool dir) {
    struct programs *p = ctx;
    char bare[PATH_LEN];
    snprintf(bare, sizeof bare, "%s", name);
    if (p->apps) { if (!dir || name[0] == '.') return; }
    else {
        if (dir) return;
        size_t n = strlen(bare);
        if (n > 4 && !strcmp(bare + n - 4, ".elf")) bare[n - 4] = '\0';
    }
    if (!strncmp(bare, p->prefix, strlen(p->prefix))) offer(p->m, bare, bare, false);
}

static int by_name(const void *a, const void *b) {
    return strcmp(((const struct edit_item *)a)->show, ((const struct edit_item *)b)->show);
}

void sh_complete(void *ctx, const char *line, size_t cur, struct edit_matches *m) {
    struct mrsh_state *state = ctx;
    char quote = 0;
    size_t qstart = 0;
    for (size_t i = 0; i < cur; i++) {
        if (quote != '\'' && line[i] == '\\') { i++; continue; }
        if (!quote && (line[i] == '\'' || line[i] == '"')) { quote = line[i]; qstart = i; }
        else if (quote && line[i] == quote) quote = 0;
    }
    size_t start = cur;
    char word[PATH_LEN];
    size_t n = 0;
    if (quote) {
        start = qstart;
        for (size_t i = qstart + 1; i < cur && n + 1 < sizeof word; i++) word[n++] = line[i];
    } else {
        while (start > 0 && !(strchr(" \t\n;&|()<>", line[start - 1]) && !(start > 1 && line[start - 2] == '\\'))) start--;
        for (size_t i = start; i < cur && n + 1 < sizeof word; i++) {
            if (line[i] == '\\' && i + 1 < cur) i++;
            word[n++] = line[i];
        }
    }
    word[n] = '\0';
    m->start = start;

    size_t p = start;
    while (p > 0 && (line[p - 1] == ' ' || line[p - 1] == '\t')) p--;
    bool command = p == 0 || strchr("\n;&|(", line[p - 1]) != NULL;

    if (!quote && word[0] == '$') {
        extern char **environ;
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            if (!eq) continue;
            char insert[160];
            snprintf(insert, sizeof insert, "$%.*s", (int)(eq - *e), *e);
            if (!strncmp(insert, word, strlen(word))) offer(m, insert, insert, false);
        }
        (void)state;
    } else if (command && !quote && !strchr(word, '/')) {
        static const char *const builtins[] = {
            ".", ":", "alias", "bg", "break", "cd", "command", "continue", "eval", "exec", "exit",
            "export", "false", "fg", "getopts", "hash", "jobs", "pwd", "read", "readonly", "return",
            "set", "shift", "times", "trap", "true", "type", "ulimit", "umask", "unalias", "unset",
            "wait", "echo", "printf", "test", "cat", "sleep", "help",
            "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac",
        };
        for (size_t i = 0; i < sizeof builtins / sizeof *builtins; i++)
            if (!strncmp(builtins[i], word, n)) offer(m, builtins[i], builtins[i], false);
        struct programs bin = { m, word, false }, apps = { m, word, true };
        each_entry("/system/bin", offer_program, &bin);
        each_entry("/data/apps", offer_program, &apps);
    } else {
        const char *slash = strrchr(word, '/');
        char typed_dir[PATH_LEN] = "", where[PATH_LEN * 2];
        const char *base = word;
        if (slash) {
            size_t d = (size_t)(slash - word) + 1;
            memcpy(typed_dir, word, d);
            typed_dir[d] = '\0';
            base = slash + 1;
        }
        if (typed_dir[0] == '/') snprintf(where, sizeof where, "%s", typed_dir);
        else {
            char cwd[PATH_LEN];
            if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
            snprintf(where, sizeof where, "%s/%s", cwd, typed_dir);
        }
        struct files f = { m, typed_dir, base, quote != 0, quote };
        each_entry(where, offer_file, &f);
    }
    if (m->n > 1) qsort(m->items, m->n, sizeof *m->items, by_name);
}
