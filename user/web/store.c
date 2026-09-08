/* user/web/store.c -- see store.h. */
#include <string.h>
#include <stdio.h>

#include "store.h"

#define ORIGIN_MAX      8
#define KEYS_PER_ORIGIN 32
#define ORIGIN_LEN     96
#define KEY_LEN        64
#define VAL_LEN       512

/* Set by the app, because only it can read $HOME -- and the same reasoning as
 * vellum.ns: this is the USER's data, so it lives in the user's home and not
 * beside the program. Empty means no persistence. */
static char g_dir[192];
void store_set_dir(const char *d) { snprintf(g_dir, sizeof g_dir, "%s", d ? d : ""); }

static struct origin {
    int  used;
    char host[ORIGIN_LEN];
    struct { int used; char key[KEY_LEN]; char val[VAL_LEN]; } kv[KEYS_PER_ORIGIN];
} g_org[ORIGIN_MAX];

static struct store_io g_io;

void store_set_io(const struct store_io *io) {
    if (io) g_io = *io; else memset(&g_io, 0, sizeof g_io);
}

void store_reset(void) { memset(g_org, 0, sizeof g_org); }

static struct origin *origin_for(const char *host, int create) {
    const char *h = host ? host : "";
    for (int i = 0; i < ORIGIN_MAX; i++)
        if (g_org[i].used && !strcmp(g_org[i].host, h)) return &g_org[i];
    if (!create) return 0;
    for (int i = 0; i < ORIGIN_MAX; i++)
        if (!g_org[i].used) {
            g_org[i].used = 1;
            snprintf(g_org[i].host, sizeof g_org[i].host, "%s", h);
            return &g_org[i];
        }
    return 0;      /* full: a ninth site simply gets no storage */
}

const char *store_get(const char *origin, const char *key) {
    struct origin *o = origin_for(origin, 0);
    if (!o || !key) return 0;
    for (int i = 0; i < KEYS_PER_ORIGIN; i++)
        if (o->kv[i].used && !strcmp(o->kv[i].key, key)) return o->kv[i].val;
    return 0;
}

int store_set(const char *origin, const char *key, const char *value) {
    if (!key || !value || strlen(key) >= KEY_LEN || strlen(value) >= VAL_LEN) return -1;
    struct origin *o = origin_for(origin, 1);
    if (!o) return -1;
    int free_slot = -1;
    for (int i = 0; i < KEYS_PER_ORIGIN; i++) {
        if (o->kv[i].used && !strcmp(o->kv[i].key, key)) {
            snprintf(o->kv[i].val, sizeof o->kv[i].val, "%s", value);
            return 0;
        }
        if (!o->kv[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return -1;
    o->kv[free_slot].used = 1;
    snprintf(o->kv[free_slot].key, sizeof o->kv[free_slot].key, "%s", key);
    snprintf(o->kv[free_slot].val, sizeof o->kv[free_slot].val, "%s", value);
    return 0;
}

int store_remove(const char *origin, const char *key) {
    struct origin *o = origin_for(origin, 0);
    if (!o || !key) return -1;
    for (int i = 0; i < KEYS_PER_ORIGIN; i++)
        if (o->kv[i].used && !strcmp(o->kv[i].key, key)) {
            memset(&o->kv[i], 0, sizeof o->kv[i]);
            return 0;
        }
    return -1;
}

int store_clear(const char *origin) {
    struct origin *o = origin_for(origin, 0);
    if (!o) return -1;
    memset(o->kv, 0, sizeof o->kv);
    return 0;
}

int store_count(const char *origin) {
    struct origin *o = origin_for(origin, 0);
    if (!o) return 0;
    int n = 0;
    for (int i = 0; i < KEYS_PER_ORIGIN; i++) if (o->kv[i].used) n++;
    return n;
}

const char *store_key_at(const char *origin, int index) {
    struct origin *o = origin_for(origin, 0);
    if (!o || index < 0) return 0;
    for (int i = 0; i < KEYS_PER_ORIGIN; i++)
        if (o->kv[i].used && index-- == 0) return o->kv[i].key;
    return 0;
}

/* --- persistence --------------------------------------------------------- *
 *
 * A line-oriented text file, because it has to be readable by a person when
 * something goes wrong and because a binary format would need a version number
 * the first time it changed. Values are escaped so a newline in a stored string
 * cannot forge a record -- a page can put anything it likes in localStorage,
 * including the file's own syntax.
 */
static size_t esc(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 2 < cap; s++) {
        if (*s == '\n')      { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*s == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else                  out[o++] = *s;
    }
    out[o] = 0;
    return o;
}

static void unesc(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            out[o++] = (s[i] == 'n') ? '\n' : s[i];
        } else out[o++] = s[i];
    }
    out[o] = 0;
}

int store_put_blob(const char *name, const char *data, size_t len) {
    if (!g_io.write || !name) return -1;
    char path[256];
    if (!g_dir[0]) return -1;
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    return g_io.write(path, data, len) < 0 ? -1 : 0;
}

long store_get_blob(const char *name, char *out, size_t cap) {
    if (!g_io.read || !name) return -1;
    char path[256];
    if (!g_dir[0]) return -1;
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    return g_io.read(path, out, cap);
}

#define STORE_FILE_MAX (64 * 1024)

int store_save(void) {
    static char buf[STORE_FILE_MAX];
    size_t n = 0;
    for (int i = 0; i < ORIGIN_MAX; i++) {
        if (!g_org[i].used) continue;
        for (int k = 0; k < KEYS_PER_ORIGIN; k++) {
            if (!g_org[i].kv[k].used) continue;
            char ek[KEY_LEN * 2], ev[VAL_LEN * 2];
            esc(g_org[i].kv[k].key, ek, sizeof ek);
            esc(g_org[i].kv[k].val, ev, sizeof ev);
            int w = snprintf(buf + n, sizeof buf - n, "%s\t%s\t%s\n",
                             g_org[i].host, ek, ev);
            if (w < 0 || (size_t)w >= sizeof buf - n) break;
            n += (size_t)w;
        }
    }
    return store_put_blob("localstorage.kv", buf, n);
}

int store_load(void) {
    static char buf[STORE_FILE_MAX];
    long got = store_get_blob("localstorage.kv", buf, sizeof buf - 1);
    if (got <= 0) return -1;
    buf[got] = 0;
    for (char *l = buf; l && *l; ) {
        char *eol = strchr(l, '\n');
        size_t len = eol ? (size_t)(eol - l) : strlen(l);
        char *t1 = memchr(l, '\t', len);
        if (t1) {
            size_t hl = (size_t)(t1 - l);
            char *t2 = memchr(t1 + 1, '\t', len - hl - 1);
            if (t2) {
                char host[ORIGIN_LEN], key[KEY_LEN], val[VAL_LEN];
                if (hl < sizeof host) {
                    memcpy(host, l, hl); host[hl] = 0;
                    unesc(t1 + 1, (size_t)(t2 - t1 - 1), key, sizeof key);
                    unesc(t2 + 1, len - (size_t)(t2 - l) - 1, val, sizeof val);
                    store_set(host, key, val);
                }
            }
        }
        if (!eol) break;
        l = eol + 1;
    }
    return 0;
}
