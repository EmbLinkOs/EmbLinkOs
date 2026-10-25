/* ==========================================================================
 * hist.c -- see hist.h. A ring of owned strings (head = next write slot,
 * count saturates), plus an append-only file.
 * ========================================================================== */
#include "hist/hist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char  *g_hist[HIST_MAX];
static size_t g_head;    /* next slot to write */
static size_t g_count;   /* live entries, saturates at HIST_MAX */
static char   g_path[512];

/* logical index (0 = oldest) -> ring slot */
static size_t slot_of(size_t i) {
    return (g_head - g_count + i + 2 * HIST_MAX) % HIST_MAX;
}

static void remember(const char *line) {
    char *copy = strdup(line);
    if (!copy) return;   /* out of memory: the line just is not remembered */
    free(g_hist[g_head]);  /* the oldest falls off (NULL until the ring is full) */
    g_hist[g_head] = copy;
    g_head = (g_head + 1) % HIST_MAX;
    if (g_count < HIST_MAX)
        g_count++;
}

/* One entry as one file line: backslash and newline escaped. */
static void append(FILE *f, const char *line) {
    for (const char *p = line; *p; p++) {
        if (*p == '\\') fputs("\\\\", f);
        else if (*p == '\n') fputs("\\n", f);
        else fputc(*p, f);
    }
    fputc('\n', f);
}

void hist_push(const char *line) {
    if (!line || line[0] == '\0')
        return;   /* never store an empty line */

    /* ignoredups: collapse a repeat of the most recent entry */
    if (g_count > 0 && strcmp(g_hist[slot_of(g_count - 1)], line) == 0)
        return;

    remember(line);
    if (g_path[0]) {
        FILE *f = fopen(g_path, "a");
        if (f) { append(f, line); fclose(f); }
    }
}

size_t hist_count(void) { return g_count; }

const char *hist_get(size_t i) {
    if (i >= g_count)
        return 0;
    return g_hist[slot_of(i)];
}

void hist_reset(void) {
    for (size_t i = 0; i < HIST_MAX; i++) { free(g_hist[i]); g_hist[i] = 0; }
    g_head = g_count = 0;
    g_path[0] = '\0';
}

/* Read one escaped line; returns 0 at end of file. *buf grows as needed. */
static int read_entry(FILE *f, char **buf, size_t *cap) {
    size_t n = 0;
    int c, escaped = 0, any = 0;
    while ((c = fgetc(f)) != EOF) {
        any = 1;
        if (!escaped && c == '\n') break;
        if (!escaped && c == '\\') { escaped = 1; continue; }
        if (escaped) { c = c == 'n' ? '\n' : c; escaped = 0; }
        if (n + 2 > *cap) {
            size_t want = *cap ? *cap * 2 : 256;
            char *grown = realloc(*buf, want);
            if (!grown) return 0;
            *buf = grown; *cap = want;
        }
        (*buf)[n++] = (char)c;
    }
    if (!any) return 0;
    if (!*buf) { *buf = malloc(1); if (!*buf) return 0; *cap = 1; }
    (*buf)[n] = '\0';
    return 1;
}

int hist_open(const char *path) {
    if (!path || strlen(path) >= sizeof g_path) return -1;
    int loaded = 0;
    size_t lines = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char *buf = 0; size_t cap = 0;
        while (read_entry(f, &buf, &cap)) {
            lines++;
            if (buf[0] && !(g_count && strcmp(g_hist[slot_of(g_count - 1)], buf) == 0)) {
                remember(buf);
                loaded++;
            }
        }
        free(buf);
        fclose(f);
    }
    snprintf(g_path, sizeof g_path, "%s", path);

    /* A file appended to for years is trimmed back to what the ring holds,
     * through a temporary file and a rename, so a crash leaves one or the
     * other, never half of each. */
    if (lines > 2 * HIST_MAX) {
        char tmp[540];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
        FILE *t = fopen(tmp, "w");
        if (t) {
            for (size_t i = 0; i < g_count; i++) append(t, hist_get(i));
            if (fclose(t) == 0) rename(tmp, g_path);
            else remove(tmp);
        }
    }
    return loaded;
}
