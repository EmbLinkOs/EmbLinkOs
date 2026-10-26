/* ==========================================================================
 * gs_index.c -- the index (Documentation/gitformat-index.txt), versions 2, 3
 * and 4: each entry's stat data, mode, object, stage and flags, and its path
 * (whole, or in v4 the previous one's with a prefix cut and a suffix added).
 * Extensions are skipped; a split or sparse index is not read.
 * ========================================================================== */
#include "gs.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool gs_index_read(const char *path, struct gs_index *out) {
    memset(out, 0, sizeof *out);
    struct stat st;
    if (stat(path, &st)) return true;                   /* no index: nothing staged, everything untracked */
    out->mtime = st.st_mtime;
#if defined(__APPLE__)
    out->mtime_ns = st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    out->mtime_ns = st.st_mtim.tv_nsec;
#endif
    struct gs_buf b = { 0 };
    if (!gs_read_file(path, &b) || b.n < 12 + 20 || memcmp(b.p, "DIRC", 4)) { gs_buf_free(&b); return false; }
    uint32_t version = be32(b.p + 4), count = be32(b.p + 8);
    if (version < 2 || version > 4) { gs_buf_free(&b); return false; }
    out->e = calloc(count ? count : 1, sizeof *out->e);
    if (!out->e) { gs_buf_free(&b); return false; }
    const uint8_t *p = b.p + 12, *end = b.p + b.n - 20;
    char *prev = NULL;
    size_t prev_len = 0;
    bool ok = true;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 62 > end) { ok = false; break; }
        struct gs_entry *e = &out->e[out->n];
        e->mtime_s = be32(p + 8);
        e->mtime_ns = be32(p + 12);
        e->ino = be32(p + 28);
        e->mode = be32(p + 24);
        e->size = be32(p + 36);
        memcpy(e->id.b, p + 40, 20);
        uint16_t flags = be16(p + 60);
        e->assume_unchanged = flags & 0x8000;
        e->stage = flags >> 12 & 3;
        const uint8_t *q = p + 62;
        if (flags & 0x4000) {                           /* extended flags, v3 and up */
            if (version < 3 || q + 2 > end) { ok = false; break; }
            uint16_t ext = be16(q);
            e->skip_worktree = ext & 0x4000;
            e->intent_to_add = ext & 0x2000;
            q += 2;
        }
        if (version == 4) {                             /* a cut from the previous path, then a suffix */
            size_t cut = 0;
            uint8_t c;
            if (q >= end) { ok = false; break; }
            c = *q++;
            cut = c & 0x7f;
            while (c & 0x80) {                          /* git's offset varint */
                if (q >= end) { ok = false; break; }
                c = *q++;
                cut = ((cut + 1) << 7) | (c & 0x7f);
            }
            if (!ok) break;
            const uint8_t *nul = memchr(q, 0, (size_t)(end - q));
            if (!nul || cut > prev_len) { ok = false; break; }
            size_t keep = prev_len - cut, suffix = (size_t)(nul - q);
            e->path = malloc(keep + suffix + 1);
            if (!e->path) { ok = false; break; }
            if (keep) memcpy(e->path, prev, keep);
            memcpy(e->path + keep, q, suffix);
            e->path[keep + suffix] = 0;
            q = nul + 1;
        } else {
            const uint8_t *nul = memchr(q, 0, (size_t)(end - q));
            if (!nul) { ok = false; break; }
            size_t len = (size_t)(nul - q);
            e->path = malloc(len + 1);
            if (!e->path) { ok = false; break; }
            memcpy(e->path, q, len + 1);
            size_t entry = (size_t)(nul - p) + 1;       /* padded with NULs to a multiple of 8 */
            q = p + ((entry + 7) & ~(size_t)7);
        }
        prev = e->path;
        prev_len = strlen(prev);
        out->n++;
        p = q;
    }
    gs_buf_free(&b);
    if (!ok) { gs_index_free(out); return false; }
    return true;
}

void gs_index_free(struct gs_index *ix) {
    for (size_t i = 0; i < ix->n; i++) free(ix->e[i].path);
    free(ix->e);
    memset(ix, 0, sizeof *ix);
}
