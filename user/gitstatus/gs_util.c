/* ==========================================================================
 * gs_util.c -- bytes, files and object names; the configuration; refs,
 * packed and loose, the tags at a commit, the stash. See gs.h.
 * ========================================================================== */
#include "gs.h"
#include "sha1.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- bytes and files ---------------------------------------------------------- */

bool gs_buf_add(struct gs_buf *b, const void *data, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) return false;
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
    return true;
}
bool gs_buf_str(struct gs_buf *b, const char *s) { return gs_buf_add(b, s, strlen(s)); }
void gs_buf_free(struct gs_buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

bool gs_read_file(const char *path, struct gs_buf *out) {
    out->n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char chunk[16384];
    size_t got;
    bool ok = gs_buf_add(out, "", 0);
    while (ok && (got = fread(chunk, 1, sizeof chunk, f)) > 0) ok = gs_buf_add(out, chunk, got);
    fclose(f);
    return ok;
}

bool gs_read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return true;
}

bool gs_exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }
bool gs_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }

void gs_join(char *out, size_t cap, const char *a, const char *b) {
    size_t n = strlen(a);
    snprintf(out, cap, "%s%s%s", a, n && a[n - 1] == '/' ? "" : "/", b);
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool gs_oid_parse(const char *hex, struct gs_oid *out) {
    for (int i = 0; i < 20; i++) {
        int h = hexval((unsigned char)hex[2 * i]), l = h < 0 ? -1 : hexval((unsigned char)hex[2 * i + 1]);
        if (h < 0 || l < 0) return false;
        out->b[i] = (uint8_t)(h << 4 | l);
    }
    return true;
}
void gs_oid_hex(const struct gs_oid *o, char out[41]) { sha1_hex(o->b, out); }
bool gs_oid_eq(const struct gs_oid *a, const struct gs_oid *b) { return !memcmp(a->b, b->b, 20); }
bool gs_oid_zero(const struct gs_oid *o) { static const uint8_t z[20]; return !memcmp(o->b, z, 20); }

void gs_hash_blob(const void *data, size_t n, struct gs_oid *out) {
    char head[32];
    int hl = snprintf(head, sizeof head, "blob %zu", n);
    struct sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, head, (size_t)hl + 1);             /* with its NUL */
    sha1_update(&c, data, n);
    sha1_final(&c, out->b);
}

/* --- configuration ---------------------------------------------------------------
 * git-config's syntax: [section], [section "subsection"], the old
 * [section.subsection], key = value, a key alone (true), "quotes", \escapes,
 * # and ; comments, a backslash continuing the line. */

static void config_set(struct gs_config *c, const char *key, const char *value) {
    for (size_t i = 0; i < c->n; i++)
        if (!strcmp(c->key[i], key)) {
            char *v = strdup(value);
            if (v) { free(c->value[i]); c->value[i] = v; }
            return;
        }
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        char **k = realloc(c->key, cap * sizeof *k), **v;
        if (!k) return;
        c->key = k;
        v = realloc(c->value, cap * sizeof *v);
        if (!v) return;
        c->value = v;
        c->cap = cap;
    }
    c->key[c->n] = strdup(key);
    c->value[c->n] = strdup(value);
    if (c->key[c->n] && c->value[c->n]) c->n++;
    else { free(c->key[c->n]); free(c->value[c->n]); }
}

void gs_config_read(struct gs_config *c, const char *path) {
    struct gs_buf b = { 0 };
    if (!gs_read_file(path, &b)) { gs_buf_free(&b); return; }
    const char *s = (const char *)b.p, *end = s + b.n;
    char section[256] = "";
    while (s < end) {
        while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
        if (s >= end) break;
        if (*s == '#' || *s == ';') { while (s < end && *s != '\n') s++; continue; }
        if (*s == '[') {
            s++;
            size_t n = 0;
            while (s < end && *s != ']' && *s != '"' && !isspace((unsigned char)*s) && n + 1 < sizeof section)
                section[n++] = (char)tolower((unsigned char)*s++);
            while (s < end && isspace((unsigned char)*s)) s++;
            if (s < end && *s == '"') {                 /* [section "subsection"]: kept as written */
                s++;
                if (n + 1 < sizeof section) section[n++] = '.';
                while (s < end && *s != '"' && n + 1 < sizeof section) {
                    if (*s == '\\' && s + 1 < end) s++;
                    section[n++] = *s++;
                }
                if (s < end) s++;
            }
            section[n] = 0;
            while (s < end && *s != ']' && *s != '\n') s++;
            if (s < end && *s == ']') s++;
            continue;
        }
        char key[256];
        size_t kn = 0;
        while (s < end && (isalnum((unsigned char)*s) || *s == '-') && kn + 1 < sizeof key) key[kn++] = (char)tolower((unsigned char)*s++);
        key[kn] = 0;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        struct gs_buf v = { 0 };
        if (s < end && *s == '=') {
            s++;
            bool quoted = false;
            size_t keep = 0;                            /* trailing space outside quotes is dropped */
            while (s < end && (*s == ' ' || *s == '\t')) s++;
            while (s < end && *s != '\n') {
                char ch = *s++;
                if (ch == '\r') continue;
                if (ch == '"') { quoted = !quoted; keep = v.n; continue; }
                if (!quoted && (ch == '#' || ch == ';')) { while (s < end && *s != '\n') s++; break; }
                if (ch == '\\' && s < end) {
                    char e = *s++;
                    if (e == '\n') continue;           /* a continued line */
                    ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'b' ? '\b' : e;
                    gs_buf_add(&v, &ch, 1);
                    keep = v.n;
                    continue;
                }
                gs_buf_add(&v, &ch, 1);
                if (quoted || (ch != ' ' && ch != '\t')) keep = v.n;
            }
            v.n = keep;
            gs_buf_add(&v, "", 0);
        } else gs_buf_str(&v, "true");
        if (kn && section[0]) {
            char full[600];
            snprintf(full, sizeof full, "%s.%s", section, key);
            config_set(c, full, (const char *)v.p);
        }
        gs_buf_free(&v);
        while (s < end && *s != '\n') s++;
    }
    gs_buf_free(&b);
}

const char *gs_config_get(const struct gs_config *c, const char *key) {
    for (size_t i = c->n; i-- > 0;) if (!strcmp(c->key[i], key)) return c->value[i];
    return NULL;
}

bool gs_config_bool(const struct gs_config *c, const char *key, bool def) {
    const char *v = gs_config_get(c, key);
    if (!v) return def;
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1")) return true;
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off") || !strcmp(v, "0") || !v[0]) return false;
    return def;
}

void gs_config_free(struct gs_config *c) {
    for (size_t i = 0; i < c->n; i++) { free(c->key[i]); free(c->value[i]); }
    free(c->key); free(c->value);
    memset(c, 0, sizeof *c);
}

/* --- refs ---------------------------------------------------------------------------- */

/* packed-refs: "<sha> <name>" lines, "^<sha>" the peeled object of the tag above. */
static bool packed_lookup(const struct gs_repo *r, const char *name, struct gs_oid *out) {
    char path[GS_PATH];
    gs_join(path, sizeof path, r->commondir, "packed-refs");
    struct gs_buf b = { 0 };
    bool found = false;
    if (gs_read_file(path, &b)) {
        size_t nl = strlen(name);
        for (char *line = (char *)b.p; line && *line && !found;) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (line[0] != '#' && line[0] != '^' && strlen(line) > 41 && line[40] == ' ' &&
                strlen(line + 41) == nl && !memcmp(line + 41, name, nl))
                found = gs_oid_parse(line, out);
            line = next;
        }
    }
    gs_buf_free(&b);
    return found;
}

bool gs_ref_resolve(struct gs_repo *r, const char *name, struct gs_oid *out, char *symref, size_t cap) {
    char cur[GS_PATH];
    snprintf(cur, sizeof cur, "%s", name);
    if (symref && cap) symref[0] = 0;
    for (int depth = 0; depth < 8; depth++) {
        /* HEAD and the worktree's own refs are the gitdir's; the rest the common dir's */
        char path[GS_PATH * 2], line[GS_PATH];
        bool own = !strcmp(cur, "HEAD") || !strncmp(cur, "refs/bisect/", 12) || !strncmp(cur, "refs/worktree/", 14);
        snprintf(path, sizeof path, "%s/%s", own ? r->gitdir : r->commondir, cur);
        if (gs_read_line(path, line, sizeof line)) {
            if (!strncmp(line, "ref:", 4)) {
                const char *t = line + 4;
                while (*t == ' ') t++;
                snprintf(cur, sizeof cur, "%s", t);
                if (symref && cap) snprintf(symref, cap, "%s", cur);
                continue;
            }
            return gs_oid_parse(line, out);
        }
        return packed_lookup(r, cur, out);
    }
    return false;
}

/* A tag ref's object, peeled through annotated tags to what they tag. */
static bool peel(struct gs_repo *r, struct gs_oid id, struct gs_oid *out) {
    for (int depth = 0; depth < 8; depth++) {
        int type;
        struct gs_buf b = { 0 };
        if (!gs_odb_read(r->odb, &id, &type, &b)) { gs_buf_free(&b); return false; }
        if (type != GS_TAG) { gs_buf_free(&b); *out = id; return true; }
        bool ok = b.n > 47 && !strncmp((const char *)b.p, "object ", 7) && gs_oid_parse((const char *)b.p + 7, &id);
        gs_buf_free(&b);
        if (!ok) return false;
    }
    return false;
}

static void consider_tag(const char *name, const struct gs_oid *peeled, const struct gs_oid *commit, char *best, size_t cap) {
    if (gs_oid_eq(peeled, commit) && strcmp(name, best) > 0) snprintf(best, cap, "%s", name);
}
/* A packed tag that also has a loose file is the loose one's (it was moved). */
static bool loose_twin(const struct gs_repo *r, const char *name) {
    char path[GS_PATH * 2];
    snprintf(path, sizeof path, "%s/refs/tags/%s", r->commondir, name);
    return gs_exists(path);
}

static void loose_tags(struct gs_repo *r, const char *dir, const char *prefix, const struct gs_oid *commit,
                       char *best, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char path[GS_PATH * 2], name[GS_PATH];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(name, sizeof name, "%s%s", prefix, e->d_name);
        if (gs_is_dir(path)) {
            char sub[GS_PATH + 2];
            snprintf(sub, sizeof sub, "%s/", name);
            loose_tags(r, path, sub, commit, best, cap);
            continue;
        }
        char line[128];
        struct gs_oid id, peeled;
        if (gs_read_line(path, line, sizeof line) && gs_oid_parse(line, &id) && peel(r, id, &peeled))
            consider_tag(name, &peeled, commit, best, cap);
    }
    closedir(d);
}

bool gs_tag_at(struct gs_repo *r, const struct gs_oid *commit, char *out, size_t cap) {
    out[0] = 0;
    char path[GS_PATH];
    /* packed tags: their peeled lines say what an annotated tag points at */
    gs_join(path, sizeof path, r->commondir, "packed-refs");
    struct gs_buf b = { 0 };
    if (gs_read_file(path, &b)) {
        char last[GS_PATH] = "";
        struct gs_oid last_id;
        bool have = false;
        for (char *line = (char *)b.p; line && *line;) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (line[0] == '^') {
                struct gs_oid peeled;
                if (have && !loose_twin(r, last) && gs_oid_parse(line + 1, &peeled)) consider_tag(last, &peeled, commit, out, cap);
                have = false;
            } else {
                if (have) {                               /* the one before had no peeled line */
                    struct gs_oid peeled;
                    if (!loose_twin(r, last) && peel(r, last_id, &peeled)) consider_tag(last, &peeled, commit, out, cap);
                }
                have = line[0] != '#' && strlen(line) > 51 && !strncmp(line + 41, "refs/tags/", 10) &&
                       gs_oid_parse(line, &last_id);
                if (have) snprintf(last, sizeof last, "%s", line + 51);
            }
            line = next;
        }
        if (have) {
            struct gs_oid peeled;
            if (!loose_twin(r, last) && peel(r, last_id, &peeled)) consider_tag(last, &peeled, commit, out, cap);
        }
    }
    gs_buf_free(&b);
    /* loose tags */
    gs_join(path, sizeof path, r->commondir, "refs/tags");
    loose_tags(r, path, "", commit, out, cap);
    return out[0] != 0;
}

int gs_stash_count(struct gs_repo *r) {
    char path[GS_PATH];
    gs_join(path, sizeof path, r->commondir, "logs/refs/stash");
    struct gs_buf b = { 0 };
    int n = 0;
    if (gs_read_file(path, &b))
        for (size_t i = 0; i < b.n; i++) n += b.p[i] == '\n';
    gs_buf_free(&b);
    return n;
}
