/* ==========================================================================
 * gs_odb.c -- git's object database, read: loose objects (zlib-deflated
 * "<type> <size>\0" + content) and packs (Documentation/gitformat-pack.txt):
 * an .idx v2 names each object's offset in its .pack, where it is whole or a
 * delta against another (by offset or by name). Alternates are followed.
 * Recently resolved pack objects are kept, since trees share delta bases.
 * ========================================================================== */
#include "gs.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

struct pack {
    FILE *f;
    uint8_t *idx;              /* the whole .idx */
    size_t idx_len;
    uint32_t count;
    const uint8_t *fanout, *names, *offs32, *offs64;
};

#define KEEP 64                /* resolved objects kept: (pack, offset) -> content */
struct kept { struct pack *pack; uint64_t off; int type; struct gs_buf data; unsigned used; };

struct gs_odb {
    char dir[GS_PATH];
    struct pack *packs;
    size_t npacks;
    struct gs_odb *alt[8];     /* alternates */
    size_t nalt;
    time_t pack_dir_mtime;
    struct kept kept[KEEP];
    unsigned clock;
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* --- packs ------------------------------------------------------------------- */

static void packs_close(struct gs_odb *o) {
    for (size_t i = 0; i < o->npacks; i++) { if (o->packs[i].f) fclose(o->packs[i].f); free(o->packs[i].idx); }
    free(o->packs);
    o->packs = NULL;
    o->npacks = 0;
    for (int i = 0; i < KEEP; i++) { gs_buf_free(&o->kept[i].data); o->kept[i].pack = NULL; }
}

static void packs_open(struct gs_odb *o) {
    char dir[GS_PATH];
    gs_join(dir, sizeof dir, o->dir, "pack");
    struct stat st;
    o->pack_dir_mtime = stat(dir, &st) == 0 ? st.st_mtime : 0;
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".idx")) continue;
        char idx[GS_PATH * 2], pack[GS_PATH * 2];
        snprintf(idx, sizeof idx, "%s/%s", dir, e->d_name);
        snprintf(pack, sizeof pack, "%s/%.*s.pack", dir, (int)(n - 4), e->d_name);
        struct gs_buf b = { 0 };
        if (!gs_read_file(idx, &b) || b.n < 8 + 256 * 4 || memcmp(b.p, "\377tOc", 4) || be32(b.p + 4) != 2) {
            gs_buf_free(&b);
            continue;                                  /* v1 indexes predate 2006: not read */
        }
        struct pack p = { 0 };
        p.idx = b.p;
        p.idx_len = b.n;
        p.fanout = b.p + 8;
        p.count = be32(p.fanout + 255 * 4);
        p.names = p.fanout + 256 * 4;
        p.offs32 = p.names + (size_t)p.count * 20 + (size_t)p.count * 4;   /* past the CRCs */
        p.offs64 = p.offs32 + (size_t)p.count * 4;
        if ((size_t)(p.offs64 - b.p) > b.n || !(p.f = fopen(pack, "rb"))) { free(b.p); continue; }
        struct pack *grown = realloc(o->packs, (o->npacks + 1) * sizeof *grown);
        if (!grown) { fclose(p.f); free(b.p); continue; }
        o->packs = grown;
        o->packs[o->npacks++] = p;
    }
    closedir(d);
}

/* A pack that has been repacked away (gc) or added: look again. */
static void packs_fresh(struct gs_odb *o) {
    char dir[GS_PATH];
    gs_join(dir, sizeof dir, o->dir, "pack");
    struct stat st;
    time_t m = stat(dir, &st) == 0 ? st.st_mtime : 0;
    if (m != o->pack_dir_mtime) { packs_close(o); packs_open(o); }
}

static bool pack_find(const struct pack *p, const struct gs_oid *id, uint64_t *off) {
    uint32_t lo = id->b[0] ? be32(p->fanout + (id->b[0] - 1) * 4) : 0, hi = be32(p->fanout + id->b[0] * 4);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = memcmp(p->names + (size_t)mid * 20, id->b, 20);
        if (!c) {
            uint32_t o32 = be32(p->offs32 + (size_t)mid * 4);
            if (o32 & 0x80000000u) {
                const uint8_t *q = p->offs64 + (size_t)(o32 & 0x7fffffffu) * 8;
                if ((size_t)(q + 8 - p->idx) > p->idx_len) return false;
                *off = (uint64_t)be32(q) << 32 | be32(q + 4);
            } else *off = o32;
            return true;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

/* Inflate from the pack's file at `at` until `size` bytes have come out. */
static bool inflate_at(FILE *f, uint64_t at, size_t size, struct gs_buf *out) {
    out->n = 0;
    if (!gs_buf_add(out, NULL, 0)) return false;
    if (size + 1 > out->cap) {
        uint8_t *p = realloc(out->p, size + 1);
        if (!p) return false;
        out->p = p;
        out->cap = size + 1;
    }
    if (fseek(f, (long)at, SEEK_SET)) return false;
    z_stream z = { 0 };
    if (inflateInit(&z) != Z_OK) return false;
    uint8_t in[16384];
    z.next_out = out->p;
    z.avail_out = (uInt)size;
    int r = Z_OK;
    while (r == Z_OK) {
        if (!z.avail_in) {
            size_t got = fread(in, 1, sizeof in, f);
            if (!got) break;
            z.next_in = in;
            z.avail_in = (uInt)got;
        }
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_BUF_ERROR && !z.avail_out) break;
    }
    bool ok = (r == Z_STREAM_END || !z.avail_out) && z.total_out == size;
    inflateEnd(&z);
    out->n = size;
    out->p[size] = 0;
    return ok;
}

static bool delta_apply(const struct gs_buf *base, const struct gs_buf *delta, struct gs_buf *out) {
    const uint8_t *d = delta->p, *end = d + delta->n;
    size_t src = 0, dst = 0;
    int shift = 0;
    do { if (d >= end) return false; src |= (size_t)(*d & 0x7f) << shift; shift += 7; } while (*d++ & 0x80);
    shift = 0;
    do { if (d >= end) return false; dst |= (size_t)(*d & 0x7f) << shift; shift += 7; } while (*d++ & 0x80);
    if (src != base->n) return false;
    out->n = 0;
    while (d < end) {
        uint8_t op = *d++;
        if (op & 0x80) {                                /* copy from the base */
            size_t off = 0, len = 0;
            for (int i = 0; i < 4; i++) if (op & (1 << i)) { if (d >= end) return false; off |= (size_t)*d++ << (8 * i); }
            for (int i = 0; i < 3; i++) if (op & (0x10 << i)) { if (d >= end) return false; len |= (size_t)*d++ << (8 * i); }
            if (!len) len = 0x10000;
            if (off + len > base->n || !gs_buf_add(out, base->p + off, len)) return false;
        } else if (op) {                                /* insert literal bytes */
            if (d + op > end || !gs_buf_add(out, d, op)) return false;
            d += op;
        } else return false;
    }
    return out->n == dst;
}

static bool pack_read(struct gs_odb *o, struct pack *p, uint64_t off, int *type, struct gs_buf *out, int depth);

static struct kept *kept_find(struct gs_odb *o, const struct pack *p, uint64_t off) {
    for (int i = 0; i < KEEP; i++) if (o->kept[i].pack == p && o->kept[i].off == off) { o->kept[i].used = ++o->clock; return &o->kept[i]; }
    return NULL;
}
static void kept_put(struct gs_odb *o, struct pack *p, uint64_t off, int type, const struct gs_buf *data) {
    struct kept *k = &o->kept[0];
    for (int i = 1; i < KEEP; i++) if (o->kept[i].used < k->used) k = &o->kept[i];
    k->data.n = 0;
    if (!gs_buf_add(&k->data, data->p, data->n)) { k->pack = NULL; return; }
    k->pack = p;
    k->off = off;
    k->type = type;
    k->used = ++o->clock;
}

static bool pack_read(struct gs_odb *o, struct pack *p, uint64_t off, int *type, struct gs_buf *out, int depth) {
    struct kept *k = kept_find(o, p, off);
    if (k) { *type = k->type; out->n = 0; return gs_buf_add(out, k->data.p, k->data.n); }
    if (depth > 64 || fseek(p->f, (long)off, SEEK_SET)) return false;
    uint8_t hdr[32];
    size_t got = fread(hdr, 1, sizeof hdr, p->f), i = 0;
    if (!got) return false;
    int t = hdr[0] >> 4 & 7;
    size_t size = hdr[0] & 15;
    int shift = 4;
    while (hdr[i] & 0x80) {
        if (++i >= got) return false;
        size |= (size_t)(hdr[i] & 0x7f) << shift;
        shift += 7;
    }
    i++;
    bool ok;
    if (t == 6 || t == 7) {                             /* OFS_DELTA, REF_DELTA */
        struct gs_buf base = { 0 }, delta = { 0 };
        int btype = 0;
        if (t == 6) {
            if (i >= got) return false;
            uint64_t rel = hdr[i] & 0x7f;
            while (hdr[i] & 0x80) {
                if (++i >= got) return false;
                rel = ((rel + 1) << 7) | (hdr[i] & 0x7f);
            }
            i++;
            ok = rel <= off && pack_read(o, p, off - rel, &btype, &base, depth + 1);
        } else {
            if (i + 20 > got) return false;
            struct gs_oid bid;
            memcpy(bid.b, hdr + i, 20);
            i += 20;
            uint64_t boff;
            ok = pack_find(p, &bid, &boff) ? pack_read(o, p, boff, &btype, &base, depth + 1)
                                           : gs_odb_read(o, &bid, &btype, &base);
        }
        ok = ok && inflate_at(p->f, off + i, size, &delta) && delta_apply(&base, &delta, out);
        *type = btype;
        gs_buf_free(&base);
        gs_buf_free(&delta);
    } else {
        *type = t;
        ok = t >= 1 && t <= 4 && inflate_at(p->f, off + i, size, out);
    }
    if (ok) kept_put(o, p, off, *type, out);
    return ok;
}

/* --- loose objects -------------------------------------------------------------- */

static bool loose_read(struct gs_odb *o, const struct gs_oid *id, int *type, struct gs_buf *out) {
    char hex[41], path[GS_PATH + 64];
    gs_oid_hex(id, hex);
    snprintf(path, sizeof path, "%s/%.2s/%s", o->dir, hex, hex + 2);
    struct gs_buf raw = { 0 };
    if (!gs_read_file(path, &raw)) { gs_buf_free(&raw); return false; }
    z_stream z = { 0 };
    bool ok = inflateInit(&z) == Z_OK;
    struct gs_buf all = { 0 };
    if (ok) {
        z.next_in = raw.p;
        z.avail_in = (uInt)raw.n;
        uint8_t chunk[16384];
        int r;
        do {
            z.next_out = chunk;
            z.avail_out = sizeof chunk;
            r = inflate(&z, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) break;
            gs_buf_add(&all, chunk, sizeof chunk - z.avail_out);
        } while (r != Z_STREAM_END);
        ok = r == Z_STREAM_END;
        inflateEnd(&z);
    }
    gs_buf_free(&raw);
    const char *h = (const char *)all.p;
    const char *nul = ok && all.n ? memchr(h, 0, all.n) : NULL;
    if (!nul) { gs_buf_free(&all); return false; }
    *type = !strncmp(h, "commit ", 7) ? GS_COMMIT : !strncmp(h, "tree ", 5) ? GS_TREE :
            !strncmp(h, "blob ", 5) ? GS_BLOB : !strncmp(h, "tag ", 4) ? GS_TAG : 0;
    size_t skip = (size_t)(nul - h) + 1;
    out->n = 0;
    ok = *type && gs_buf_add(out, all.p + skip, all.n - skip);
    gs_buf_free(&all);
    return ok;
}

/* --- the database ------------------------------------------------------------------ */

static struct gs_odb *odb_open(const char *dir, int depth) {
    struct gs_odb *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    snprintf(o->dir, sizeof o->dir, "%s", dir);
    packs_open(o);
    char path[GS_PATH];
    gs_join(path, sizeof path, dir, "info/alternates");
    struct gs_buf b = { 0 };
    if (depth < 4 && gs_read_file(path, &b))
        for (char *line = (char *)b.p; line && *line && o->nalt < 8;) {
            char *next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (line[0] && line[0] != '#') {
                char alt[GS_PATH];
                if (line[0] == '/') snprintf(alt, sizeof alt, "%s", line);
                else gs_join(alt, sizeof alt, dir, line);
                struct gs_odb *a = odb_open(alt, depth + 1);
                if (a) o->alt[o->nalt++] = a;
            }
            line = next;
        }
    gs_buf_free(&b);
    return o;
}

struct gs_odb *gs_odb_open(const char *objects_dir) { return odb_open(objects_dir, 0); }

void gs_odb_close(struct gs_odb *o) {
    if (!o) return;
    packs_close(o);
    for (size_t i = 0; i < o->nalt; i++) gs_odb_close(o->alt[i]);
    free(o);
}

bool gs_odb_read(struct gs_odb *o, const struct gs_oid *id, int *type, struct gs_buf *out) {
    packs_fresh(o);
    for (size_t i = 0; i < o->npacks; i++) {
        uint64_t off;
        if (pack_find(&o->packs[i], id, &off)) return pack_read(o, &o->packs[i], off, type, out, 0);
    }
    if (loose_read(o, id, type, out)) return true;
    for (size_t i = 0; i < o->nalt; i++) if (gs_odb_read(o->alt[i], id, type, out)) return true;
    return false;
}
