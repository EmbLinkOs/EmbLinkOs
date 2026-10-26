/* On-disk repository writer -- see repo.h. */
#include "repo.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static void mkdirp(const char *path) { mkdir(path, 0755); }   /* EEXIST is fine */

static int write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    return (w == len) ? 0 : -1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void hex2bin(const char *hex, uint8_t out[20]) {
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)((hexval(hex[2*i]) << 4) | hexval(hex[2*i+1]));
}

int repo_init(const char *dir) {
    char p[1024];
    mkdirp(dir);
    snprintf(p, sizeof p, "%s/.git", dir);            mkdirp(p);
    snprintf(p, sizeof p, "%s/.git/objects", dir);    mkdirp(p);
    snprintf(p, sizeof p, "%s/.git/refs", dir);       mkdirp(p);
    snprintf(p, sizeof p, "%s/.git/refs/heads", dir); mkdirp(p);
    return 0;
}

int repo_write_objects(const char *dir, const struct pack_obj *objs, int n) {
    int written = 0;
    for (int i = 0; i < n; i++) {
        const struct pack_obj *o = &objs[i];
        /* Loose object = "<type> <size>\0" + content, then zlib-deflated. */
        char hdr[40];
        int hl = snprintf(hdr, sizeof hdr, "%s %zu", pack_type_name(o->type), o->size);
        size_t fulllen = (size_t)hl + 1 + o->size;    /* +1 for the NUL */
        uint8_t *full = malloc(fulllen);
        if (!full) return -1;
        memcpy(full, hdr, (size_t)hl); full[hl] = 0;
        memcpy(full + hl + 1, o->data, o->size);

        uLongf clen = compressBound(fulllen);
        uint8_t *comp = malloc(clen);
        if (!comp || compress(comp, &clen, full, fulllen) != Z_OK) { free(full); free(comp); return -1; }
        free(full);

        char hex[41]; sha1_hex(o->sha, hex);
        char d[1100], path[1200];
        snprintf(d, sizeof d, "%s/.git/objects/%c%c", dir, hex[0], hex[1]);
        mkdirp(d);
        snprintf(path, sizeof path, "%s/%s", d, hex + 2);
        int rc = write_file(path, comp, clen);
        free(comp);
        if (rc != 0) return -1;
        written++;
    }
    return written;
}

int repo_write_refs(const char *dir, const char *branch, const char *head_hex) {
    char p[1024], buf[256];
    int n = snprintf(buf, sizeof buf, "ref: refs/heads/%s\n", branch);
    snprintf(p, sizeof p, "%s/.git/HEAD", dir);
    if (write_file(p, buf, (size_t)n) != 0) return -1;

    n = snprintf(buf, sizeof buf, "%s\n", head_hex);
    snprintf(p, sizeof p, "%s/.git/refs/heads/%s", dir, branch);
    if (write_file(p, buf, (size_t)n) != 0) return -1;

    const char *cfg = "[core]\n\trepositoryformatversion = 0\n\tbare = false\n";
    snprintf(p, sizeof p, "%s/.git/config", dir);
    return write_file(p, cfg, strlen(cfg));
}

int repo_write_shallow(const char *dir, char shas[][41], int n) {
    if (n <= 0) return 0;
    char buf[8 * 42]; size_t o = 0;
    for (int i = 0; i < n && o + 42 <= sizeof buf; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s\n", shas[i]);
    char p[1024];
    snprintf(p, sizeof p, "%s/.git/shallow", dir);
    return write_file(p, buf, o);
}

static const struct pack_obj *find_obj(const struct pack_obj *objs, int n, const uint8_t sha[20]) {
    for (int i = 0; i < n; i++) if (memcmp(objs[i].sha, sha, 20) == 0) return &objs[i];
    return NULL;
}

/* What the checkout wrote, for the index: path from the work tree's top, mode, object. */
struct ix_entry { char path[512]; uint32_t mode; uint8_t sha[20]; };
struct ix_list { struct ix_entry *e; int n, cap; };

static int ix_add(struct ix_list *l, const char *path, uint32_t mode, const uint8_t sha[20]) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        struct ix_entry *e = realloc(l->e, (size_t)cap * sizeof *e);
        if (!e) return -1;
        l->e = e;
        l->cap = cap;
    }
    snprintf(l->e[l->n].path, sizeof l->e[l->n].path, "%s", path);
    l->e[l->n].mode = mode;
    memcpy(l->e[l->n].sha, sha, 20);
    l->n++;
    return 0;
}

/* Recursively write a tree object's entries into `dir` (`rel`: its path from
 * the top, "" or "a/b/"): files, links as links, a submodule's empty
 * directory; each noted for the index. */
static int checkout_tree(const char *dir, const char *rel, const struct pack_obj *objs, int n,
                         const uint8_t tree_sha[20], int *files, struct ix_list *ix) {
    const struct pack_obj *t = find_obj(objs, n, tree_sha);
    if (!t || t->type != OBJ_TREE) return -1;

    const uint8_t *p = t->data, *end = t->data + t->size;
    while (p < end) {
        /* entry: "<octal mode> <name>\0" + 20 raw sha bytes */
        const uint8_t *sp = memchr(p, ' ', (size_t)(end - p));
        if (!sp) break;
        char mode[8]; size_t ml = (size_t)(sp - p);
        if (ml >= sizeof mode) break;
        memcpy(mode, p, ml); mode[ml] = 0;

        const uint8_t *nul = memchr(sp + 1, 0, (size_t)(end - (sp + 1)));
        if (!nul) break;
        char name[256]; size_t nl = (size_t)(nul - (sp + 1));
        if (nl >= sizeof name) break;
        memcpy(name, sp + 1, nl); name[nl] = 0;

        const uint8_t *esha = nul + 1;
        if (esha + 20 > end) break;

        char path[1400], sub[512];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        snprintf(sub, sizeof sub, "%s%s", rel, name);
        uint32_t m = (uint32_t)strtoul(mode, NULL, 8);
        if (m == 040000) {                                                  /* subtree */
            char subrel[520];
            snprintf(subrel, sizeof subrel, "%s/", sub);
            mkdirp(path);
            if (checkout_tree(path, subrel, objs, n, esha, files, ix) != 0) return -1;
        } else if (m == 0160000) {                                          /* a submodule: not fetched */
            mkdirp(path);
            if (ix_add(ix, sub, m, esha) != 0) return -1;
        } else {                                                            /* blob: a file or a link */
            const struct pack_obj *b = find_obj(objs, n, esha);
            if (b && b->type == OBJ_BLOB) {
                if (m == 0120000) {
                    char target[1024];
                    size_t tl = b->size < sizeof target - 1 ? b->size : sizeof target - 1;
                    memcpy(target, b->data, tl); target[tl] = 0;
                    if (symlink(target, path) != 0 && write_file(path, b->data, b->size) != 0) return -1;
                } else {
                    if (write_file(path, b->data, b->size) != 0) return -1;
                    if (m == 0100755) (void)chmod(path, 0755);   /* where the filesystem keeps it */
                }
                (*files)++;
                if (ix_add(ix, sub, m == 0100755 || m == 0120000 ? m : 0100644, esha) != 0) return -1;
            }
        }
        p = esha + 20;
    }
    return 0;
}

static int ix_cmp(const void *a, const void *b) {
    return strcmp(((const struct ix_entry *)a)->path, ((const struct ix_entry *)b)->path);
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* .git/index, version 2 (Documentation/gitformat-index.txt): every entry
 * with the stat data the checkout left, so status reads the tree clean. */
static int write_index(const char *dir, struct ix_list *ix) {
    qsort(ix->e, (size_t)ix->n, sizeof *ix->e, ix_cmp);
    size_t cap = 12 + (size_t)ix->n * (62 + 512 + 8) + 20, o = 12;
    uint8_t *buf = calloc(1, cap);
    if (!buf) return -1;
    memcpy(buf, "DIRC", 4);
    put32(buf + 4, 2);
    put32(buf + 8, (uint32_t)ix->n);
    for (int i = 0; i < ix->n; i++) {
        const struct ix_entry *e = &ix->e[i];
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, e->path);
        struct stat st;
        memset(&st, 0, sizeof st);
        if (e->mode != 0160000) (void)lstat(path, &st);
        uint8_t *q = buf + o;
        put32(q + 0, (uint32_t)st.st_ctime);
        put32(q + 8, (uint32_t)st.st_mtime);
        put32(q + 16, (uint32_t)st.st_dev);
        put32(q + 20, (uint32_t)st.st_ino);
        put32(q + 24, e->mode);
        put32(q + 36, e->mode == 0160000 ? 0 : (uint32_t)st.st_size);
        memcpy(q + 40, e->sha, 20);
        size_t nl = strlen(e->path);
        q[60] = (uint8_t)((nl > 0xfff ? 0xfff : nl) >> 8);
        q[61] = (uint8_t)(nl > 0xfff ? 0xff : nl);
        memcpy(q + 62, e->path, nl);
        o += (62 + nl + 8) & ~(size_t)7;                 /* NUL-padded to a multiple of 8 */
    }
    sha1(buf, o, buf + o);
    char p[1024];
    snprintf(p, sizeof p, "%s/.git/index", dir);
    int rc = write_file(p, buf, o + 20);
    free(buf);
    return rc;
}

int repo_checkout(const char *dir, const struct pack_obj *objs, int n,
                  const char *head_hex) {
    uint8_t csha[20]; hex2bin(head_hex, csha);
    const struct pack_obj *commit = find_obj(objs, n, csha);
    if (!commit || commit->type != OBJ_COMMIT) return -1;

    /* Commit body starts "tree <40hex>\n". */
    if (commit->size < 46 || memcmp(commit->data, "tree ", 5) != 0) return -1;
    char thex[41]; memcpy(thex, commit->data + 5, 40); thex[40] = 0;
    uint8_t tsha[20]; hex2bin(thex, tsha);

    int files = 0;
    struct ix_list ix = { 0 };
    int rc = checkout_tree(dir, "", objs, n, tsha, &files, &ix);
    if (rc == 0) rc = write_index(dir, &ix);
    free(ix.e);
    return rc != 0 ? -1 : files;
}
