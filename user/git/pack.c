/* Packfile unpacking -- see pack.h. */
#include "pack.h"
#include "sha1.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { OBJ_OFS_DELTA = 6, OBJ_REF_DELTA = 7 };

const char *pack_type_name(int type) {
    switch (type) {
    case OBJ_COMMIT: return "commit";
    case OBJ_TREE:   return "tree";
    case OBJ_BLOB:   return "blob";
    case OBJ_TAG:    return "tag";
    default:         return "?";
    }
}

/* One raw pack entry (pre-delta-resolution). */
struct entry {
    size_t   offset;           /* byte offset of this entry within the pack */
    int      type;             /* OBJ_* or OBJ_OFS_DELTA / OBJ_REF_DELTA */
    uint8_t *data;             /* inflated payload (object content, or delta) */
    size_t   size;             /* inflated size */
    size_t   base_off;         /* OFS_DELTA: absolute offset of the base */
    uint8_t  base_sha[20];     /* REF_DELTA: base object id */
    uint8_t  sha[20];          /* this object's id, once resolved */
    int      resolved;         /* 1 once type/data are a real object */
};

/* Inflate the zlib stream at src (<= srcmax bytes) into out (exactly outsize).
 * Returns input bytes consumed, or -1. */
static long zinflate(const uint8_t *src, size_t srcmax, uint8_t *out, size_t outsize) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return -1;
    z.next_in = (unsigned char *)src; z.avail_in = (uInt)srcmax;
    z.next_out = out; z.avail_out = (uInt)outsize;
    int r = inflate(&z, Z_FINISH);
    long consumed = (long)(srcmax - z.avail_in);
    int done = (r == Z_STREAM_END && z.total_out == outsize);
    inflateEnd(&z);
    return done ? consumed : -1;
}

/* Object header varint: byte0 = [cont|type(3)|size(4)], then 7 bits each. */
static const uint8_t *read_obj_header(const uint8_t *p, const uint8_t *end,
                                      int *type, size_t *size) {
    if (p >= end) return NULL;
    uint8_t b = *p++;
    *type = (b >> 4) & 7;
    size_t s = b & 0x0f; int shift = 4;
    while (b & 0x80) {
        if (p >= end) return NULL;
        b = *p++;
        s |= (size_t)(b & 0x7f) << shift; shift += 7;
    }
    *size = s;
    return p;
}

/* OFS-delta base distance varint (the "offset encoding"). */
static const uint8_t *read_ofs(const uint8_t *p, const uint8_t *end, size_t *val) {
    if (p >= end) return NULL;
    uint8_t b = *p++;
    size_t v = b & 0x7f;
    while (b & 0x80) {
        if (p >= end) return NULL;
        b = *p++;
        v = ((v + 1) << 7) | (b & 0x7f);
    }
    *val = v;
    return p;
}

/* A copy/insert delta applied to `base` -> malloc'd `*out` (size *outlen). */
static int apply_delta(const uint8_t *base, size_t baselen,
                       const uint8_t *delta, size_t deltalen,
                       uint8_t **out, size_t *outlen) {
    const uint8_t *p = delta, *end = delta + deltalen;
    size_t src_size = 0, dst_size = 0; int shift;
    for (shift = 0; p < end; shift += 7) { uint8_t b = *p++; src_size |= (size_t)(b & 0x7f) << shift; if (!(b & 0x80)) break; }
    for (shift = 0; p < end; shift += 7) { uint8_t b = *p++; dst_size |= (size_t)(b & 0x7f) << shift; if (!(b & 0x80)) break; }
    if (src_size != baselen) return -1;

    uint8_t *dst = malloc(dst_size ? dst_size : 1);
    if (!dst) return -1;
    size_t o = 0;
    while (p < end) {
        uint8_t op = *p++;
        if (op & 0x80) {                       /* copy from base */
            size_t off = 0, len = 0;
            if (op & 0x01) off |= (size_t)(p < end ? *p++ : 0);
            if (op & 0x02) off |= (size_t)(p < end ? *p++ : 0) << 8;
            if (op & 0x04) off |= (size_t)(p < end ? *p++ : 0) << 16;
            if (op & 0x08) off |= (size_t)(p < end ? *p++ : 0) << 24;
            if (op & 0x10) len |= (size_t)(p < end ? *p++ : 0);
            if (op & 0x20) len |= (size_t)(p < end ? *p++ : 0) << 8;
            if (op & 0x40) len |= (size_t)(p < end ? *p++ : 0) << 16;
            if (len == 0) len = 0x10000;
            if (off + len > baselen || o + len > dst_size) { free(dst); return -1; }
            memcpy(dst + o, base + off, len); o += len;
        } else if (op) {                       /* insert literal from delta */
            if (p + op > end || o + op > dst_size) { free(dst); return -1; }
            memcpy(dst + o, p, op); p += op; o += op;
        } else { free(dst); return -1; }       /* op == 0 is reserved */
    }
    if (o != dst_size) { free(dst); return -1; }
    *out = dst; *outlen = dst_size;
    return 0;
}

static void object_sha(int type, const uint8_t *data, size_t size, uint8_t out[20]) {
    char hdr[32];
    int hl = 0; const char *tn = pack_type_name(type);
    while (*tn) hdr[hl++] = *tn++;
    hdr[hl++] = ' ';
    /* decimal size */
    char num[24]; int nn = 0; size_t s = size;
    if (s == 0) num[nn++] = '0';
    while (s) { num[nn++] = (char)('0' + s % 10); s /= 10; }
    for (int i = nn - 1; i >= 0; i--) hdr[hl++] = num[i];
    hdr[hl++] = '\0';
    struct sha1_ctx c; sha1_init(&c);
    sha1_update(&c, hdr, (size_t)hl);
    sha1_update(&c, data, size);
    sha1_final(&c, out);
}

static struct entry *find_by_offset(struct entry *e, int n, size_t off) {
    for (int i = 0; i < n; i++) if (e[i].offset == off) return &e[i];
    return NULL;
}
static struct entry *find_by_sha(struct entry *e, int n, const uint8_t sha[20]) {
    for (int i = 0; i < n; i++) if (e[i].resolved && memcmp(e[i].sha, sha, 20) == 0) return &e[i];
    return NULL;
}

int pack_unpack(const uint8_t *pack, size_t len, struct pack_obj **out) {
    if (len < 12 + 20 || memcmp(pack, "PACK", 4) != 0) return -1;
    uint32_t nobj = ((uint32_t)pack[8] << 24) | (pack[9] << 16) | (pack[10] << 8) | pack[11];
    if (nobj == 0 || nobj > 1000000) return -1;

    struct entry *e = calloc(nobj, sizeof *e);
    if (!e) return -1;
    const uint8_t *p = pack + 12, *end = pack + len - 20;   /* stop before the trailer */

    for (uint32_t i = 0; i < nobj; i++) {
        e[i].offset = (size_t)(p - pack);
        int t; size_t sz;
        const uint8_t *q = read_obj_header(p, end, &t, &sz);
        if (!q) goto fail;
        e[i].type = t; e[i].size = sz;

        if (t == OBJ_OFS_DELTA) {
            size_t d; q = read_ofs(q, end, &d); if (!q) goto fail;
            e[i].base_off = e[i].offset - d;
        } else if (t == OBJ_REF_DELTA) {
            if (q + 20 > end) goto fail;
            memcpy(e[i].base_sha, q, 20); q += 20;
        }
        /* Inflate the (object content | delta) that follows. */
        uint8_t *buf = malloc(sz ? sz : 1); if (!buf) goto fail;
        long used = zinflate(q, (size_t)(end - q), buf, sz);
        if (used < 0) { free(buf); goto fail; }
        e[i].data = buf;
        p = q + used;
    }

    /* Resolve deltas: repeated passes until every entry is a real object. */
    for (uint32_t i = 0; i < nobj; i++)
        if (e[i].type >= OBJ_COMMIT && e[i].type <= OBJ_TAG) {
            object_sha(e[i].type, e[i].data, e[i].size, e[i].sha);
            e[i].resolved = 1;
        }
    int remaining = 0; for (uint32_t i = 0; i < nobj; i++) if (!e[i].resolved) remaining++;
    while (remaining) {
        int progressed = 0;
        for (uint32_t i = 0; i < nobj; i++) {
            if (e[i].resolved) continue;
            struct entry *base = (e[i].type == OBJ_OFS_DELTA)
                ? find_by_offset(e, (int)nobj, e[i].base_off)
                : find_by_sha(e, (int)nobj, e[i].base_sha);
            if (!base || !base->resolved) continue;
            uint8_t *full; size_t fulllen;
            if (apply_delta(base->data, base->size, e[i].data, e[i].size, &full, &fulllen) != 0) goto fail;
            free(e[i].data);
            e[i].data = full; e[i].size = fulllen; e[i].type = base->type;
            object_sha(e[i].type, e[i].data, e[i].size, e[i].sha);
            e[i].resolved = 1; remaining--; progressed = 1;
        }
        if (!progressed) goto fail;                /* missing base / cycle */
    }

    struct pack_obj *objs = calloc(nobj, sizeof *objs);
    if (!objs) goto fail;
    for (uint32_t i = 0; i < nobj; i++) {
        objs[i].type = e[i].type; objs[i].data = e[i].data;
        objs[i].size = e[i].size; memcpy(objs[i].sha, e[i].sha, 20);
    }
    free(e);
    *out = objs;
    return (int)nobj;

fail:
    for (uint32_t i = 0; i < nobj; i++) free(e[i].data);
    free(e);
    return -1;
}
