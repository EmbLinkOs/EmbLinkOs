/* ==========================================================================
 * gs_ignore.c -- gitignore (Documentation/gitignore.txt): git's wildmatch,
 * and a file's patterns with their forms -- `#` comments, `!` to re-include,
 * a trailing `/` for directories only, a `/` elsewhere anchoring the pattern
 * to the file's directory, `**` across directories.
 * ========================================================================== */
#include "gs.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- wildmatch ------------------------------------------------------------- */

enum { WM_NOMATCH, WM_MATCH, WM_ABORT_ALL, WM_ABORT_TO_STARSTAR };

static bool is_special(unsigned char c) { return c == '*' || c == '?' || c == '[' || c == '\\'; }

static bool in_class(const unsigned char *s, int n, unsigned char t) {
    static const struct { const char *name; int (*fn)(int); } classes[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank }, { "cntrl", iscntrl }, { "digit", isdigit },
        { "graph", isgraph }, { "lower", islower }, { "print", isprint }, { "punct", ispunct }, { "space", isspace },
        { "upper", isupper }, { "xdigit", isxdigit } };
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if ((int)strlen(classes[i].name) == n && !strncmp(classes[i].name, (const char *)s, (size_t)n)) return classes[i].fn(t) != 0;
    return false;
}

/* git's dowild (wildmatch.c), WM_PATHNAME when `pathname`. */
static int dowild(const unsigned char *p, const unsigned char *text, bool pathname, const unsigned char *pattern) {
    unsigned char p_ch;
    for (; (p_ch = *p) != '\0'; text++, p++) {
        int matched, match_slash, negated;
        unsigned char t_ch = *text, prev_ch;
        if (t_ch == '\0' && p_ch != '*') return WM_ABORT_ALL;
        switch (p_ch) {
        case '\\':
            p_ch = *++p;
            /* fall through */
        default:
            if (t_ch != p_ch) return WM_NOMATCH;
            continue;
        case '?':
            if (pathname && t_ch == '/') return WM_NOMATCH;
            continue;
        case '*':
            if (*++p == '*') {
                const unsigned char *prev_p = p - 2;
                while (*++p == '*') {}
                if ((prev_p < pattern || *prev_p == '/') && (*p == '\0' || *p == '/' || (p[0] == '\\' && p[1] == '/'))) {
                    /* "**" between slashes: also nothing at all */
                    if (p[0] == '/' && dowild(p + 1, text, pathname, pattern) == WM_MATCH) return WM_MATCH;
                    match_slash = 1;
                } else match_slash = !pathname;
            } else match_slash = !pathname;
            if (*p == '\0') {
                if (!match_slash && strchr((const char *)text, '/')) return WM_ABORT_TO_STARSTAR;
                return WM_MATCH;
            } else if (!match_slash && *p == '/') {
                const char *slash = strchr((const char *)text, '/');
                if (!slash) return WM_ABORT_ALL;
                text = (const unsigned char *)slash;
                break;                                  /* the loop takes the slash */
            }
            for (;;) {
                if (t_ch == '\0') break;
                if (!is_special(*p)) {                  /* to the next place the literal could be */
                    p_ch = *p;
                    while ((t_ch = *text) != '\0' && (match_slash || t_ch != '/')) {
                        if (t_ch == p_ch) break;
                        text++;
                    }
                    if (t_ch != p_ch) return match_slash ? WM_ABORT_ALL : WM_ABORT_TO_STARSTAR;
                }
                if ((matched = dowild(p, text, pathname, pattern)) != WM_NOMATCH) {
                    if (!match_slash || matched != WM_ABORT_TO_STARSTAR) return matched;
                } else if (!match_slash && t_ch == '/') return WM_ABORT_TO_STARSTAR;
                t_ch = *++text;
            }
            return WM_ABORT_ALL;
        case '[':
            p_ch = *++p;
            if (p_ch == '^') p_ch = '!';
            negated = p_ch == '!';
            if (negated) p_ch = *++p;
            prev_ch = 0;
            matched = 0;
            do {
                if (!p_ch) return WM_ABORT_ALL;
                if (p_ch == '\\') {
                    p_ch = *++p;
                    if (!p_ch) return WM_ABORT_ALL;
                    if (t_ch == p_ch) matched = 1;
                } else if (p_ch == '-' && prev_ch && p[1] && p[1] != ']') {
                    p_ch = *++p;
                    if (p_ch == '\\') { p_ch = *++p; if (!p_ch) return WM_ABORT_ALL; }
                    if (t_ch <= p_ch && t_ch >= prev_ch) matched = 1;
                    p_ch = 0;
                } else if (p_ch == '[' && p[1] == ':') {
                    const unsigned char *s;
                    for (s = p += 2; (p_ch = *p) && p_ch != ']'; p++) {}
                    if (!p_ch) return WM_ABORT_ALL;
                    int i = (int)(p - s - 1);
                    if (i < 0 || p[-1] != ':') {        /* no ":]": an ordinary set after all */
                        p = s - 2;
                        p_ch = '[';
                        if (t_ch == p_ch) matched = 1;
                        continue;
                    }
                    if (in_class(s, i, t_ch)) matched = 1;
                    p_ch = 0;
                } else if (t_ch == p_ch) matched = 1;
            } while (prev_ch = p_ch, (p_ch = *++p) != ']');
            if (matched == negated || (pathname && t_ch == '/')) return WM_NOMATCH;
            continue;
        }
    }
    return *text ? WM_NOMATCH : WM_MATCH;
}

bool gs_wildmatch(const char *pat, const char *text, bool pathname) {
    const unsigned char *p = (const unsigned char *)pat;
    return dowild(p, (const unsigned char *)text, pathname, p) == WM_MATCH;
}

/* --- a file of patterns ------------------------------------------------------------- */

void gs_rules_load(struct gs_rules *rs, const char *file, const char *base) {
    memset(rs, 0, sizeof *rs);
    snprintf(rs->base, sizeof rs->base, "%s", base);
    struct gs_buf b = { 0 };
    if (!gs_read_file(file, &b)) { gs_buf_free(&b); return; }
    size_t lines = 1;
    for (size_t i = 0; i < b.n; i++) lines += b.p[i] == '\n';
    rs->r = calloc(lines, sizeof *rs->r);
    if (!rs->r) { gs_buf_free(&b); return; }
    for (char *line = (char *)b.p; line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        /* trailing spaces go, unless escaped */
        while (n && line[n - 1] == ' ' && !(n > 1 && line[n - 2] == '\\')) line[--n] = 0;
        if (!n || line[0] == '#') { line = next; continue; }
        struct gs_rule r = { 0 };
        char *p = line;
        if (*p == '!') { r.negate = true; p++; }
        else if (*p == '\\' && (p[1] == '!' || p[1] == '#')) p++;
        n = strlen(p);
        if (n && p[n - 1] == '/') { r.dir_only = true; p[--n] = 0; }
        if (!n) { line = next; continue; }
        r.has_slash = strchr(p, '/') != NULL;
        if (*p == '/') { p++; r.anchored = true; }
        else if (r.has_slash) r.anchored = true;
        r.pat = strdup(p);
        if (r.pat) rs->r[rs->n++] = r;
        line = next;
    }
    gs_buf_free(&b);
}

void gs_rules_free(struct gs_rules *rs) {
    for (size_t i = 0; i < rs->n; i++) free(rs->r[i].pat);
    free(rs->r);
    memset(rs, 0, sizeof *rs);
}

int gs_rules_match(const struct gs_rules *rs, const char *path, bool is_dir) {
    size_t bl = strlen(rs->base);
    if (bl && strncmp(path, rs->base, bl)) return 0;    /* not below this file's directory */
    const char *rel = path + bl, *slash = strrchr(rel, '/'), *name = slash ? slash + 1 : rel;
    for (size_t i = rs->n; i-- > 0;) {                  /* the last pattern that matches decides */
        const struct gs_rule *r = &rs->r[i];
        if (r->dir_only && !is_dir) continue;
        bool hit = r->anchored ? gs_wildmatch(r->pat, rel, true) : gs_wildmatch(r->pat, name, true);
        if (hit) return r->negate ? -1 : 1;
    }
    return 0;
}
