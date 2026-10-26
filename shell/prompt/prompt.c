/* ==========================================================================
 * prompt.c -- the prompt's options, and the prompt built from them.
 * See prompt.h.
 * ========================================================================== */
#include "prompt/prompt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- options ------------------------------------------------------------- */

#define PROMPT_OPTS 256
struct opt { char key[PROMPT_KEY_MAX]; char *value; };
static struct opt g_opts[PROMPT_OPTS];
static size_t g_nopts;

bool prompt_key_valid(const char *key) {
    size_t n = key ? strlen(key) : 0;
    if (n == 0 || n >= PROMPT_KEY_MAX || key[0] == '.' || key[n - 1] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = key[i];
        bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (c == '.' ? key[i + 1] == '.' : !word) return false;
    }
    return true;
}

static struct opt *find(const char *key) {
    for (size_t i = 0; i < g_nopts; i++) if (!strcmp(g_opts[i].key, key)) return &g_opts[i];
    return NULL;
}

bool prompt_opt_set(const char *key, const char *value) {
    if (!prompt_key_valid(key) || !value || strlen(value) >= PROMPT_VALUE_MAX) return false;
    char *copy = malloc(strlen(value) + 1);
    if (!copy) return false;
    memcpy(copy, value, strlen(value) + 1);
    struct opt *o = find(key);
    if (!o) {
        if (g_nopts == PROMPT_OPTS) { free(copy); return false; }
        o = &g_opts[g_nopts++];
        memcpy(o->key, key, strlen(key) + 1);
    } else free(o->value);
    o->value = copy;
    return true;
}

const char *prompt_opt(const char *key) {
    struct opt *o = key ? find(key) : NULL;
    return o ? o->value : NULL;
}

bool prompt_opt_unset(const char *key) {
    struct opt *o = key ? find(key) : NULL;
    if (!o) return false;
    free(o->value);
    size_t i = (size_t)(o - g_opts);
    memmove(&g_opts[i], &g_opts[i + 1], (g_nopts - i - 1) * sizeof *g_opts);
    g_nopts--;
    return true;
}

void prompt_opt_reset(void) {
    for (size_t i = 0; i < g_nopts; i++) free(g_opts[i].value);
    g_nopts = 0;
}

size_t prompt_opt_count(void) { return g_nopts; }
const char *prompt_opt_key(size_t i) { return i < g_nopts ? g_opts[i].key : NULL; }
const char *prompt_opt_value(size_t i) { return i < g_nopts ? g_opts[i].value : NULL; }

/* --- the prompt ----------------------------------------------------------- */

static const char *opt_or(const char *key, const char *fallback) {
    const char *v = prompt_opt(key);
    return v && v[0] ? v : fallback;
}

/* Text appended to a fixed buffer; what does not fit is left out. */
static void add(char *buf, size_t cap, const char *s) {
    size_t n = strlen(buf), k = strlen(s);
    if (n + k >= cap) k = cap - 1 - n;
    memcpy(buf + n, s, k);
    buf[n + k] = 0;
}

/* Powerlevel10k's way: 3s, 1m 5s, 2h 0m 7s. */
static void duration_text(uint64_t ms, char *out, size_t cap) {
    uint64_t s = ms / 1000, h = s / 3600, m = s / 60 % 60;
    if (h) snprintf(out, cap, "%luh %lum %lus", (unsigned long)h, (unsigned long)m, (unsigned long)(s % 60));
    else if (m) snprintf(out, cap, "%lum %lus", (unsigned long)m, (unsigned long)(s % 60));
    else snprintf(out, cap, "%lus", (unsigned long)s);
}

static void time_text(const struct prompt_state *s, char *out, size_t cap) {
    if (!strcmp(opt_or("time.format", "24h"), "12h")) {
        int h = s->hour % 12 ? s->hour % 12 : 12;
        snprintf(out, cap, "%02d:%02d:%02d %s", h, s->minute, s->second, s->hour < 12 ? "AM" : "PM");
    } else snprintf(out, cap, "%02d:%02d:%02d", s->hour, s->minute, s->second);
}

/* One element of the right side; false when it has nothing to show now. */
static bool right_element(const struct prompt_state *s, const char *name, char *out, size_t cap) {
    out[0] = 0;
    if (!strcmp(name, "status")) {
        if (s->status < 0) return false;
        if (s->status == 0) snprintf(out, cap, "\033[32m\xe2\x9c\x94\033[0m");                /* ✔ */
        else snprintf(out, cap, "\033[31m\xe2\x9c\x98 %d\033[0m", s->status);                 /* ✘ 1 */
        return true;
    }
    if (!strcmp(name, "duration")) {
        long threshold = strtol(opt_or("duration.threshold", "3"), NULL, 10);
        if (s->status < 0 || s->duration_ms < (uint64_t)(threshold < 0 ? 0 : threshold) * 1000) return false;
        char d[32];
        duration_text(s->duration_ms, d, sizeof d);
        snprintf(out, cap, "\033[33m%s\033[0m", d);
        return true;
    }
    if (!strcmp(name, "time")) {
        char t[32];
        time_text(s, t, sizeof t);
        snprintf(out, cap, "\033[2m%s\033[0m", t);
        return true;
    }
    return false;                  /* not an element (yet): left out */
}

void prompt_build(const struct prompt_state *s, int cols, struct prompt_text *out) {
    (void)cols;                    /* the left side does not depend on the width yet */
    const char *ch = opt_or("char", "$");
    out->left[0] = out->right[0] = out->transient[0] = 0;
    snprintf(out->left, sizeof out->left, "\033[1;36m%s@%s:%s%s\033[0m ",
             s->user && s->user[0] ? s->user : "user", s->host && s->host[0] ? s->host : "emblink",
             s->dir ? s->dir : "", ch);

    /* the right side: its elements, in the order named, two spaces apart */
    for (const char *p = opt_or("right", ""); *p;) {
        while (*p == ' ' || *p == ',') p++;
        size_t n = strcspn(p, " ,");
        if (!n) break;
        char name[32], piece[96];
        snprintf(name, sizeof name, "%.*s", (int)(n < sizeof name ? n : sizeof name - 1), p);
        p += n;
        if (!right_element(s, name, piece, sizeof piece)) continue;
        if (out->right[0]) add(out->right, sizeof out->right, "  ");
        add(out->right, sizeof out->right, piece);
    }

    /* transient: the prompt character alone, green or red by how the last
     * command went -- p10k's way -- unless it is off, or (same-dir) the
     * directory has just changed */
    const char *t = opt_or("transient", "off");
    bool same = s->last_dir && s->dir && !strcmp(s->last_dir, s->dir);
    if (!strcmp(t, "always") || (!strcmp(t, "same-dir") && same))
        snprintf(out->transient, sizeof out->transient, "\033[1;%dm%s\033[0m ", s->status > 0 ? 31 : 32, ch);
}
