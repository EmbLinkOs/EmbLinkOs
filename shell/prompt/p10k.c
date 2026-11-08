/* ==========================================================================
 * p10k.c -- the prompt engine's configuration and Powerlevel10k's lookups:
 * parameters with their fallbacks, icons by mode, colours by name. See
 * p10k.h; the lookups follow p10k's _p9k_param, _p9k_get_icon and
 * _p9k_color (internal/p10k.zsh) line for line.
 * ========================================================================== */
#include "prompt/p10k_int.h"
#include "prompt/icons.h"
#include "prompt/colors.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- strings ---------------------------------------------------------------- */

static bool sb_room(struct sb *b, size_t more) {
    if (b->oom) return false;
    if (b->n + more + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + more + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) { b->oom = true; return false; }
    b->p = p; b->cap = cap;
    return true;
}
void sb_add(struct sb *b, const char *s, size_t n) {
    if (!n || !sb_room(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
void sb_str(struct sb *b, const char *s) { if (s) sb_add(b, s, strlen(s)); }
void sb_chr(struct sb *b, char c) { sb_add(b, &c, 1); }
void sb_fmt(struct sb *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
const char *sb_text(const struct sb *b) { return b->p ? b->p : ""; }
void sb_free(struct sb *b) { free(b->p); b->p = 0; b->n = b->cap = 0; b->oom = false; }
void sb_reset(struct sb *b) { b->n = 0; if (b->p) b->p[0] = 0; }

/* --- the configuration ------------------------------------------------------ */

struct param { char *name, *value; };
struct p10k { struct param *v; size_t n, cap; };

struct p10k *p10k_new(void) { return calloc(1, sizeof(struct p10k)); }
void p10k_clear(struct p10k *cfg) {
    for (size_t i = 0; i < cfg->n; i++) { free(cfg->v[i].name); free(cfg->v[i].value); }
    cfg->n = 0;
}
void p10k_free(struct p10k *cfg) {
    if (!cfg) return;
    p10k_clear(cfg);
    free(cfg->v);
    free(cfg);
}
static struct param *find(const struct p10k *cfg, const char *name) {
    for (size_t i = 0; i < cfg->n; i++) if (!strcmp(cfg->v[i].name, name)) return &cfg->v[i];
    return NULL;
}
static char *dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
bool p10k_set(struct p10k *cfg, const char *name, const char *value) {
    if (!name || !name[0] || !value) return false;
    char *v = dup(value);
    if (!v) return false;
    struct param *p = find(cfg, name);
    if (p) { free(p->value); p->value = v; return true; }
    if (cfg->n == cfg->cap) {
        size_t cap = cfg->cap ? cfg->cap * 2 : 64;
        struct param *nv = realloc(cfg->v, cap * sizeof *nv);
        if (!nv) { free(v); return false; }
        cfg->v = nv; cfg->cap = cap;
    }
    char *n = dup(name);
    if (!n) { free(v); return false; }
    cfg->v[cfg->n].name = n;
    cfg->v[cfg->n].value = v;
    cfg->n++;
    return true;
}
bool p10k_unset(struct p10k *cfg, const char *name) {
    struct param *p = find(cfg, name);
    if (!p) return false;
    free(p->name); free(p->value);
    size_t i = (size_t)(p - cfg->v);
    memmove(&cfg->v[i], &cfg->v[i + 1], (cfg->n - i - 1) * sizeof *cfg->v);
    cfg->n--;
    return true;
}
const char *p10k_get(const struct p10k *cfg, const char *name) {
    const struct param *p = cfg ? find(cfg, name) : NULL;
    return p ? p->value : NULL;
}
size_t p10k_count(const struct p10k *cfg) { return cfg->n; }
const char *p10k_name_at(const struct p10k *cfg, size_t i) { return i < cfg->n ? cfg->v[i].name : NULL; }
const char *p10k_value_at(const struct p10k *cfg, size_t i) { return i < cfg->n ? cfg->v[i].value : NULL; }

bool p9k_isset(const struct p10k *cfg, const char *name) { return p10k_get(cfg, name) != NULL; }

/* --- _p9k_param --------------------------------------------------------------
 * prompt_foo_BAR NAME: FOO_BAR_NAME, then FOO_NAME, then NAME. The segment is
 * the lower-case run after prompt_ (dashes as underscores), the state the
 * rest. */
const char *p9k_param(const struct p10k *cfg, const char *seg, const char *name, const char *def) {
    char var[160];
    if (seg && !strncmp(seg, "prompt_", 7)) {
        const char *s = seg + 7;
        size_t low = 0;
        while (s[low] && (islower((unsigned char)s[low]) || isdigit((unsigned char)s[low]) || s[low] == '_' || s[low] == '-'))
            low++;
        char lower[96];
        size_t k = 0;
        for (size_t i = 0; i < low && k + 1 < sizeof lower; i++) lower[k++] = s[i] == '-' ? '_' : (char)toupper((unsigned char)s[i]);
        lower[k] = 0;
        snprintf(var, sizeof var, "%s%s_%s", lower, s + low, name);
        const char *v = p10k_get(cfg, var);
        if (v) return v;
        if (k && lower[k - 1] == '_') lower[--k] = 0;
        snprintf(var, sizeof var, "%s_%s", lower, name);
        v = p10k_get(cfg, var);
        if (v) return v;
    }
    const char *v = p10k_get(cfg, name);
    return v ? v : def;
}

/* _p9k_declare -b: set, only "true" is true. */
bool p9k_bool(const struct p10k *cfg, const char *name, bool def) {
    const char *v = p10k_get(cfg, name);
    return v ? !strcmp(v, "true") : def;
}
long p9k_int(const struct p10k *cfg, const char *name, long def) {
    const char *v = p10k_get(cfg, name);
    if (!v || !v[0]) return def;
    char *end;
    long n = strtol(v, &end, 10);
    return end == v ? def : n;
}
double p9k_float(const struct p10k *cfg, const char *name, double def) {
    const char *v = p10k_get(cfg, name);
    if (!v || !v[0]) return def;
    char *end;
    double d = strtod(v, &end);
    return end == v ? def : d;
}

size_t p9k_array(const char *value, size_t i, char *out, size_t cap) {
    out[0] = 0;
    if (!value) return 0;
    size_t count = 0;
    const char *p = value;
    if (strchr(value, P10K_ARRAY_SEP)) {        /* elements joined by \037: empty ones count */
        for (;;) {
            const char *e = strchr(p, P10K_ARRAY_SEP);
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (count++ == i) { size_t n = len < cap - 1 ? len : cap - 1; memcpy(out, p, n); out[n] = 0; }
            if (!e) break;
            p = e + 1;
        }
        return count;
    }
    while (*p) {                                /* a plain value: words */
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (count++ == i) { size_t n = (size_t)(e - p) < cap - 1 ? (size_t)(e - p) : cap - 1; memcpy(out, p, n); out[n] = 0; }
        p = e;
    }
    return count;
}

/* --- escapes: ${(g::)x} -------------------------------------------------------- */

static void utf8(struct sb *out, unsigned long cp) {
    char b[4];
    size_t n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xc0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xe0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { b[0] = (char)(0xf0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63)); b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
    sb_add(out, b, n);
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
void p9k_decode_escapes(const char *s, struct sb *out) {
    for (const char *p = s; *p;) {
        if (*p != '\\' || !p[1]) { sb_chr(out, *p++); continue; }
        char c = p[1];
        int max = c == 'u' ? 4 : c == 'U' ? 8 : c == 'x' ? 2 : 0;
        if (max) {
            unsigned long v = 0;
            int k = 0;
            const char *q = p + 2;
            while (k < max && hexval(q[k]) >= 0) { v = v * 16 + (unsigned long)hexval(q[k]); k++; }
            if (k) { if (c == 'x') sb_chr(out, (char)v); else utf8(out, v); p = q + k; continue; }
        }
        switch (c) {
        case 'n': sb_chr(out, '\n'); break;
        case 't': sb_chr(out, '\t'); break;
        case 'e': case 'E': sb_chr(out, '\033'); break;
        case 'a': sb_chr(out, '\a'); break;
        case '\\': sb_chr(out, '\\'); break;
        default: sb_chr(out, '\\'); sb_chr(out, c); break;
        }
        p += 2;
    }
}

/* --- icons ----------------------------------------------------------------- */

static int icon_mode(const struct p10k *cfg) {
    const char *m = p10k_get(cfg, "MODE");
    if (!m || !m[0]) return P10K_ICON_DEFAULT;
    if (!strcmp(m, "flat") || !strcmp(m, "awesome-patched")) return P10K_ICON_FLAT;
    if (!strcmp(m, "awesome-fontconfig")) return P10K_ICON_AWESOME_FONTCONFIG;
    if (!strcmp(m, "nerdfont-v3")) return P10K_ICON_NERDFONT_V3;
    if (!strcmp(m, "nerdfont-complete") || !strcmp(m, "nerdfont-fontconfig")) return P10K_ICON_NERDFONT_COMPLETE;
    if (!strcmp(m, "ascii")) return P10K_ICON_ASCII;
    return P10K_ICON_DEFAULT;
}

/* The mode's icon, as _p9k_init_icons leaves it: the spacing filled in, the
 * flat and compatible overrides, and ICON_PADDING=none's trimming. */
static bool mode_icon(const struct p10k *cfg, const char *name, struct sb *out) {
    const char *mode = p10k_get(cfg, "MODE");
    int m = icon_mode(cfg);
    const char *raw = NULL;
    if (mode && !strcmp(mode, "flat")) {
        if (!strcmp(name, "LEFT_SEGMENT_SEPARATOR") || !strcmp(name, "RIGHT_SEGMENT_SEPARATOR")) raw = "";
        else if (!strcmp(name, "LEFT_SUBSEGMENT_SEPARATOR") || !strcmp(name, "RIGHT_SUBSEGMENT_SEPARATOR")) raw = "|";
    } else if (mode && !strcmp(mode, "compatible")) {
        if (!strcmp(name, "LEFT_SEGMENT_SEPARATOR")) raw = "\xe2\xae\x80";
        else if (!strcmp(name, "RIGHT_SEGMENT_SEPARATOR")) raw = "\xe2\xae\x82";
        else if (!strcmp(name, "VCS_BRANCH_ICON")) raw = "@";
    }
    if (!raw) {
        unsigned lo = 0, hi = p10k_icon_count;
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            int c = strcmp(p10k_icons[mid].name, name);
            if (!c) { raw = p10k_icons[mid].value[m]; break; }
            if (c < 0) lo = mid + 1; else hi = mid;
        }
    }
    if (!raw) return false;
    bool legacy = p9k_bool(cfg, "LEGACY_ICON_SPACING", false);
    size_t start = out->n;
    for (const char *p = raw; *p; p++) {
        if (*p == '\001') { if (!legacy) sb_chr(out, ' '); }
        else if (*p == '\002') { if (legacy) sb_chr(out, ' '); }
        else sb_chr(out, *p);
    }
    const char *pad = p10k_get(cfg, "ICON_PADDING");
    if (pad && !strcmp(pad, "none") && m != P10K_ICON_ASCII) {
        while (out->n > start && out->p[out->n - 1] == ' ') out->p[--out->n] = 0;
        static const char *const plus[] = { "LEFT_SEGMENT_END_SEPARATOR", "MULTILINE_LAST_PROMPT_PREFIX", "VCS_TAG_ICON",
                                             "VCS_BOOKMARK_ICON", "VCS_COMMIT_ICON", "VCS_BRANCH_ICON", "VCS_REMOTE_BRANCH_ICON" };
        for (size_t i = 0; i < sizeof plus / sizeof plus[0]; i++) if (!strcmp(name, plus[i])) sb_chr(out, ' ');
    }
    return true;
}

const char *p9k_icon(const struct p10k *cfg, const char *seg, const char *name, const char *def, struct sb *out) {
    sb_reset(out);
    if (name[0] == '\001') { sb_str(out, name + 1); return sb_text(out); }
    struct sb icon = { 0 };
    bool have = mode_icon(cfg, name, &icon);
    const char *v = p9k_param(cfg, seg, name, NULL);
    if (v) p9k_decode_escapes(v, out);          /* a parameter: ${(g::)...} */
    else if (have) sb_str(out, sb_text(&icon));
    else sb_str(out, def ? def : "");
    sb_free(&icon);
    return sb_text(out);
}

/* --- colours ----------------------------------------------------------------- */

void p9k_translate_color(const char *c, char out[16]) {
    out[0] = 0;
    if (!c || !c[0]) return;
    bool digits = true;
    for (const char *p = c; *p; p++) if (!isdigit((unsigned char)*p)) digits = false;
    if (digits) { snprintf(out, 16, "%03d", atoi(c)); return; }
    if (c[0] == '#') {
        size_t i = 0;
        for (; c[i] && i < 15; i++) out[i] = (char)tolower((unsigned char)c[i]);
        out[i] = 0;
        return;
    }
    if (!strncmp(c, "bg-", 3) || !strncmp(c, "fg-", 3)) c += 3;
    if (!strncmp(c, "br", 2)) c += 2;
    int n = p10k_color_named(c);
    if (n >= 0) snprintf(out, 16, "%03d", n);
}

const char *p9k_color(const struct p10k *cfg, const char *seg, const char *name, const char *def, char out[16]) {
    p9k_translate_color(p9k_param(cfg, seg, name, def), out);
    return out;
}
