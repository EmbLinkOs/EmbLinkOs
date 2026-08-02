/* On-disk repository writer -- see repo.h. */
#include "repo.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* Recursively write a tree object's entries into `dir`. */
static int checkout_tree(const char *dir, const struct pack_obj *objs, int n,
                         const uint8_t tree_sha[20], int *files) {
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

        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (strcmp(mode, "40000") == 0 || strcmp(mode, "040000") == 0) {   /* subtree */
            mkdirp(path);
            if (checkout_tree(path, objs, n, esha, files) != 0) return -1;
        } else {                                                            /* blob */
            const struct pack_obj *b = find_obj(objs, n, esha);
            if (b && b->type == OBJ_BLOB) {
                if (write_file(path, b->data, b->size) != 0) return -1;
                (*files)++;
            }
        }
        p = esha + 20;
    }
    return 0;
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
    if (checkout_tree(dir, objs, n, tsha, &files) != 0) return -1;
    return files;
}
