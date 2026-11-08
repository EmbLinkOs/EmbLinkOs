/* Terminal profiles: the presets, the parser, and a writer that keeps what it
 * does not understand. See profile.h. */
#include "profile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the presets ------------------------------------------------------------------------
 * Clear Dark is the Mac's profile this Terminal follows, as
 * tools/import_macos_terminal_profile.py converts it: the colours in sRGB, the
 * background at 95% over a blur. EmbLink is the look before profiles: the
 * system theme's text, background and accent, and the screen model's palette.
 * Solarized is Ethan Schoonover's palette (MIT licence) in its usual terminal
 * order; Basic is black on white with xterm's colours. */
static const char CLEAR_DARK[] =
    "# Imported from Terminal.app's \"Clear Dark\" profile by tools/import_macos_terminal_profile.py.\n"
    "name = Clear Dark\n"
    "font = MesloLGS Nerd Font Mono\n"
    "font_size = 14\n"
    "line_spacing = 1\n"
    "letter_spacing = 1\n"
    "bold_is_bright = yes\n"
    "foreground = #e6e6e6\n"
    "background = #212734\n"
    "bold = #f0f0f0\n"
    "selection = #334e5e\n"
    "cursor = #e6e6e6\n"
    "color0 = #35424c\ncolor1 = #b45648\ncolor2 = #6caa71\ncolor3 = #c4ac62\n"
    "color4 = #6d96b4\ncolor5 = #bd7bcd\ncolor6 = #7ccbcd\ncolor7 = #dee5eb\n"
    "color8 = #465c6d\ncolor9 = #df6c5a\ncolor10 = #79be7e\ncolor11 = #e5c872\n"
    "color12 = #67b5ed\ncolor13 = #d389e5\ncolor14 = #84dde0\ncolor15 = #e5eff5\n"
    "opacity = 0.95\n"
    "blur = 0.50\n"
    "cursor_shape = block\n"
    "cursor_blink = no\n"
    "columns = 120\n"
    "rows = 30\n"
    "bell = visual\n";

static const char EMBLINK[] =
    "name = EmbLink\n"
    "theme_colors = yes\n"
    "bold_is_bright = no\n"
    "color0 = #30343b\ncolor1 = #cf666c\ncolor2 = #73bb85\ncolor3 = #dbbf70\n"
    "color4 = #789fdb\ncolor5 = #b98bcc\ncolor6 = #6abec5\ncolor7 = #d9dfe7\n"
    "color8 = #81858e\ncolor9 = #f58b91\ncolor10 = #91dba0\ncolor11 = #f0d988\n"
    "color12 = #99bdf4\ncolor13 = #d4aaf1\ncolor14 = #8bdde2\ncolor15 = #f5f7fa\n"
    "opacity = 1\n"
    "blur = 0\n";

#define SOLARIZED_ANSI \
    "color0 = #073642\ncolor1 = #dc322f\ncolor2 = #859900\ncolor3 = #b58900\n" \
    "color4 = #268bd2\ncolor5 = #d33682\ncolor6 = #2aa198\ncolor7 = #eee8d5\n" \
    "color8 = #002b36\ncolor9 = #cb4b16\ncolor10 = #586e75\ncolor11 = #657b83\n" \
    "color12 = #839496\ncolor13 = #6c71c4\ncolor14 = #93a1a1\ncolor15 = #fdf6e3\n"

static const char SOLARIZED_DARK[] =
    "name = Solarized Dark\n"
    "bold_is_bright = no\n"
    "foreground = #839496\nbackground = #002b36\nbold = #93a1a1\nselection = #073642\ncursor = #93a1a1\n"
    SOLARIZED_ANSI
    "opacity = 1\nblur = 0\n";

static const char SOLARIZED_LIGHT[] =
    "name = Solarized Light\n"
    "bold_is_bright = no\n"
    "foreground = #657b83\nbackground = #fdf6e3\nbold = #586e75\nselection = #eee8d5\ncursor = #586e75\n"
    SOLARIZED_ANSI
    "opacity = 1\nblur = 0\n";

static const char BASIC[] =
    "name = Basic\n"
    "bold_is_bright = no\n"
    "foreground = #000000\nbackground = #ffffff\nbold = #000000\nselection = #b4d5fe\ncursor = #7f7f7f\n"
    "color0 = #000000\ncolor1 = #c23621\ncolor2 = #25bc24\ncolor3 = #adad27\n"
    "color4 = #492ee1\ncolor5 = #d338d3\ncolor6 = #33bbc8\ncolor7 = #cbcccd\n"
    "color8 = #818383\ncolor9 = #fc391f\ncolor10 = #31e722\ncolor11 = #eaec23\n"
    "color12 = #5833ff\ncolor13 = #f935f8\ncolor14 = #14f0f0\ncolor15 = #e9ebeb\n"
    "opacity = 1\nblur = 0\n";

static const char *const PRESETS[] = {CLEAR_DARK, EMBLINK, SOLARIZED_DARK, SOLARIZED_LIGHT, BASIC};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))

int profile_preset_count(void) { return NPRESETS; }
const char *profile_preset_text(int i) { return i >= 0 && i < NPRESETS ? PRESETS[i] : NULL; }

/* --- lines --------------------------------------------------------------------------- */

/* The next line of `text` from *at: its key and value, trimmed. False at the
 * end; a line that is not `key = value` (a comment, a blank) has key_len 0. */
struct kv { const char *line; size_t line_len; const char *key; size_t key_len; const char *val; size_t val_len; };
static bool next_line(const char *text, size_t *at, struct kv *kv) {
    const char *p = text + *at;
    if (!*p) return false;
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    *at += len + (end ? 1 : 0);
    memset(kv, 0, sizeof *kv);
    kv->line = p; kv->line_len = len;
    const char *s = p, *e = p + len;
    while (s < e && isspace((unsigned char)*s)) s++;
    if (s == e || *s == '#') return true;
    const char *eq = memchr(s, '=', (size_t)(e - s));
    if (!eq) return true;
    const char *ke = eq;
    while (ke > s && isspace((unsigned char)ke[-1])) ke--;
    const char *v = eq + 1, *ve = e;
    while (v < ve && isspace((unsigned char)*v)) v++;
    while (ve > v && isspace((unsigned char)ve[-1])) ve--;
    kv->key = s; kv->key_len = (size_t)(ke - s);
    kv->val = v; kv->val_len = (size_t)(ve - v);
    return kv->key_len > 0;
}
static bool key_is(const struct kv *kv, const char *key) {
    return kv->key_len == strlen(key) && !memcmp(kv->key, key, kv->key_len);
}
static void copy_value(const struct kv *kv, char *out, size_t cap) {
    size_t n = kv->val_len < cap - 1 ? kv->val_len : cap - 1;
    memcpy(out, kv->val, n); out[n] = 0;
}

const char *profile_preset_name(int i) {
    static char names[NPRESETS][PROFILE_NAME_MAX];
    if (i < 0 || i >= NPRESETS) return NULL;
    if (!names[i][0]) profile_conf_get(PRESETS[i], "name", names[i], sizeof names[i]);
    return names[i];
}
int profile_preset_find(const char *name) {
    for (int i = 0; i < NPRESETS; i++) if (!strcmp(profile_preset_name(i), name)) return i;
    return -1;
}

/* --- the keys ------------------------------------------------------------------------ */

enum kind { K_STR, K_FLOAT, K_INT, K_BOOL, K_COLOR, K_CURSOR, K_BELL };
struct key { const char *name; enum kind kind; size_t off, size; float lo, hi; };
#define F(field) offsetof(struct term_profile, field), sizeof(((struct term_profile *)0)->field)
static const struct key KEYS[] = {
    {"name", K_STR, F(name), 0, 0},
    {"font", K_STR, F(font), 0, 0},
    {"font_size", K_FLOAT, F(font_size), 6, 72},
    {"line_spacing", K_FLOAT, F(line_spacing), 0.8f, 2},
    {"letter_spacing", K_FLOAT, F(letter_spacing), 0.8f, 2},
    {"bold_is_bright", K_BOOL, F(bold_is_bright), 0, 0},
    {"theme_colors", K_BOOL, F(theme_colors), 0, 0},
    {"foreground", K_COLOR, F(fg), 0, 0},
    {"background", K_COLOR, F(bg), 0, 0},
    {"bold", K_COLOR, F(bold), 0, 0},
    {"selection", K_COLOR, F(selection), 0, 0},
    {"cursor", K_COLOR, F(cursor), 0, 0},
    {"opacity", K_FLOAT, F(opacity), 0.3f, 1},
    {"blur", K_FLOAT, F(blur), 0, 1},
    {"padding", K_INT, F(padding), 0, 48},
    {"cursor_shape", K_CURSOR, F(cursor_shape), 0, 0},
    {"cursor_blink", K_BOOL, F(cursor_blink), 0, 0},
    {"columns", K_INT, F(columns), 20, 400},
    {"rows", K_INT, F(rows), 5, 200},
    {"scrollback", K_INT, F(scrollback_mb), 1, 64},
    {"bell", K_BELL, F(bell), 0, 0},
    {"command", K_STR, F(command), 0, 0},
    {"directory", K_STR, F(directory), 0, 0},
    {"term", K_STR, F(term), 0, 0},
    {"clipboard_write", K_BOOL, F(clipboard_write), 0, 0},
};
#undef F
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))
static const char *const CURSORS[] = {"block", "underline", "bar"};
static const char *const BELLS[] = {"visual", "none"};

static bool parse_color(const char *v, uint32_t *out) {
    if (v[0] != '#' || strlen(v) != 7) return false;
    for (int i = 1; i < 7; i++) if (!isxdigit((unsigned char)v[i])) return false;
    *out = (uint32_t)strtoul(v + 1, NULL, 16);
    return true;
}
static bool parse_bool(const char *v, bool *out) {
    if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1")) { *out = true; return true; }
    if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "0")) { *out = false; return true; }
    return false;
}
static int parse_name(const char *v, const char *const *names, int n) {
    for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i;
    return -1;
}

/* One key's value into the profile; false if the value is not one it takes. */
static bool take(struct term_profile *p, const struct key *k, const char *v) {
    char *field = (char *)p + k->off;
    char *end;
    switch (k->kind) {
    case K_STR:
        if (!v[0] || strlen(v) >= k->size) return false;
        memcpy(field, v, strlen(v) + 1);
        return true;
    case K_FLOAT: {
        float f = strtof(v, &end);
        if (end == v || *end || f < k->lo || f > k->hi) return false;
        *(float *)field = f;
        return true;
    }
    case K_INT: {
        long n = strtol(v, &end, 10);
        if (end == v || *end || n < (long)k->lo || n > (long)k->hi) return false;
        *(int *)field = (int)n;
        return true;
    }
    case K_BOOL: return parse_bool(v, (bool *)field);
    case K_COLOR: return parse_color(v, (uint32_t *)field);
    case K_CURSOR: {
        int i = parse_name(v, CURSORS, 3);
        if (i < 0) return false;
        *(enum profile_cursor *)field = (enum profile_cursor)i;
        return true;
    }
    case K_BELL: {
        int i = parse_name(v, BELLS, 2);
        if (i < 0) return false;
        *(enum profile_bell *)field = (enum profile_bell)i;
        return true;
    }
    }
    return false;
}

static void put(const struct term_profile *p, const struct key *k, char *out, size_t cap) {
    const char *field = (const char *)p + k->off;
    switch (k->kind) {
    case K_STR: snprintf(out, cap, "%s", field); break;
    case K_FLOAT: snprintf(out, cap, "%g", (double)*(const float *)field); break;
    case K_INT: snprintf(out, cap, "%d", *(const int *)field); break;
    case K_BOOL: snprintf(out, cap, "%s", *(const bool *)field ? "yes" : "no"); break;
    case K_COLOR: snprintf(out, cap, "#%06x", (unsigned)(*(const uint32_t *)field & 0xffffffu)); break;
    case K_CURSOR: snprintf(out, cap, "%s", CURSORS[*(const enum profile_cursor *)field]); break;
    case K_BELL: snprintf(out, cap, "%s", BELLS[*(const enum profile_bell *)field]); break;
    }
}

/* A key's name and its value as text: the colours are color0-15, the rest
 * the table. Index 0..NKEYS-1 the table, NKEYS.. the colours. */
#define NALL (NKEYS + 16)
static void key_name(int i, char *out, size_t cap) {
    if (i < NKEYS) snprintf(out, cap, "%s", KEYS[i].name);
    else snprintf(out, cap, "color%d", i - NKEYS);
}
static void key_value(const struct term_profile *p, int i, char *out, size_t cap) {
    if (i < NKEYS) put(p, &KEYS[i], out, cap);
    else snprintf(out, cap, "#%06x", (unsigned)(p->color[i - NKEYS] & 0xffffffu));
}
static int key_find(const struct kv *kv) {
    for (int i = 0; i < NKEYS; i++) if (key_is(kv, KEYS[i].name)) return i;
    if (kv->key_len >= 6 && kv->key_len <= 7 && !memcmp(kv->key, "color", 5)) {
        int n = 0;
        for (size_t j = 5; j < kv->key_len; j++) {
            if (!isdigit((unsigned char)kv->key[j])) return -1;
            n = n * 10 + (kv->key[j] - '0');
        }
        if (n < 16 && !(kv->key_len == 7 && kv->key[5] == '0')) return NKEYS + n;
    }
    return -1;
}

/* --- reading and writing ------------------------------------------------------------- */

void profile_defaults(struct term_profile *p) {
    memset(p, 0, sizeof *p);
    snprintf(p->font, sizeof p->font, "MesloLGS Nerd Font Mono");
    p->font_size = 14; p->line_spacing = p->letter_spacing = 1;
    p->opacity = 1; p->padding = 10;
    p->columns = 120; p->rows = 30; p->scrollback_mb = 8;
    snprintf(p->command, sizeof p->command, "/system/bin/shell.elf");
    snprintf(p->directory, sizeof p->directory, "same");
    snprintf(p->term, sizeof p->term, "xterm-256color");
    profile_parse(p, CLEAR_DARK);
}

int profile_parse(struct term_profile *p, const char *text) {
    int took = 0;
    size_t at = 0;
    struct kv kv;
    while (text && next_line(text, &at, &kv)) {
        if (!kv.key_len) continue;
        int i = key_find(&kv);
        if (i < 0) continue;
        char v[256];
        copy_value(&kv, v, sizeof v);
        if (i >= NKEYS) {
            if (parse_color(v, &p->color[i - NKEYS])) took++;
        } else if (take(p, &KEYS[i], v)) took++;
    }
    return took;
}

bool profile_preset(struct term_profile *p, const char *name) {
    int i = profile_preset_find(name);
    if (i < 0) return false;
    profile_defaults(p);
    profile_parse(p, PRESETS[i]);
    return true;
}

static bool emit(char *out, size_t cap, size_t *n, const char *s, size_t len) {
    if (*n + len >= cap) return false;
    memcpy(out + *n, s, len); *n += len; out[*n] = 0;
    return true;
}

int profile_format(const struct term_profile *p, const char *old, char *out, size_t cap) {
    bool written[NALL] = {false};
    size_t n = 0, at = 0;
    struct kv kv;
    char line[320], name[32], value[256];
    if (!cap) return -1;
    out[0] = 0;
    while (old && next_line(old, &at, &kv)) {
        int i = kv.key_len ? key_find(&kv) : -1;
        if (i < 0 || written[i]) {           /* not ours, or a key said twice: as it was */
            if (i >= 0) continue;            /* the second copy of a key would win on reading: drop it */
            if (!emit(out, cap, &n, kv.line, kv.line_len) || !emit(out, cap, &n, "\n", 1)) return -1;
            continue;
        }
        written[i] = true;
        key_name(i, name, sizeof name); key_value(p, i, value, sizeof value);
        int len = snprintf(line, sizeof line, "%s = %s\n", name, value);
        if (len < 0 || !emit(out, cap, &n, line, (size_t)len)) return -1;
    }
    for (int i = 0; i < NALL; i++) {
        if (written[i]) continue;
        key_name(i, name, sizeof name); key_value(p, i, value, sizeof value);
        int len = snprintf(line, sizeof line, "%s = %s\n", name, value);
        if (len < 0 || !emit(out, cap, &n, line, (size_t)len)) return -1;
    }
    return (int)n;
}

void profile_file_name(const char *name, char *out, size_t cap) {
    size_t n = 0;
    bool dash = false;
    for (const char *s = name; *s && n + 6 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c)) {
            if (dash && n) out[n++] = '-';
            out[n++] = (char)tolower(c);
            dash = false;
        } else dash = true;
    }
    if (!n) out[n++] = 'x';
    snprintf(out + n, cap - n, ".conf");
}

bool profile_conf_get(const char *text, const char *key, char *out, size_t cap) {
    size_t at = 0;
    struct kv kv;
    bool found = false;
    while (text && next_line(text, &at, &kv))
        if (kv.key_len && key_is(&kv, key)) { copy_value(&kv, out, cap); found = true; }
    return found;
}

int profile_conf_set(const char *text, const char *key, const char *value, char *out, size_t cap) {
    size_t n = 0, at = 0;
    struct kv kv;
    bool done = false;
    char line[320];
    if (!cap) return -1;
    out[0] = 0;
    int len = snprintf(line, sizeof line, "%s = %s\n", key, value);
    if (len < 0 || len >= (int)sizeof line) return -1;
    while (text && next_line(text, &at, &kv)) {
        if (kv.key_len && key_is(&kv, key)) {
            if (!done && !emit(out, cap, &n, line, (size_t)len)) return -1;
            done = true;
            continue;
        }
        if (!emit(out, cap, &n, kv.line, kv.line_len) || !emit(out, cap, &n, "\n", 1)) return -1;
    }
    if (!done && !emit(out, cap, &n, line, (size_t)len)) return -1;
    return (int)n;
}

int profile_decscusr(const struct term_profile *p) {
    int base = p->cursor_shape == PROFILE_CURSOR_BAR ? 5 : p->cursor_shape == PROFILE_CURSOR_UNDERLINE ? 3 : 1;
    return p->cursor_blink ? base : base + 1;
}
