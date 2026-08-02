/* git push core -- see push.h. Transport-independent: object construction,
 * request serialization, and report-status parsing. */
#include "push.h"
#include "pktline.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHA-1 of a git object: "<type> <size>\0" + content (same rule as loose
 * objects and pack naming). */
static void obj_sha(const char *type, const uint8_t *data, size_t size, uint8_t out[20]) {
    char hdr[32];
    int hl = snprintf(hdr, sizeof hdr, "%s %zu", type, size);
    struct sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, hdr, (size_t)hl + 1);      /* include the terminating NUL */
    sha1_update(&c, data, size);
    sha1_final(&c, out);
}

static void hex20(const uint8_t sha[20], char out[41]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) { out[2*i] = h[sha[i] >> 4]; out[2*i+1] = h[sha[i] & 15]; }
    out[40] = 0;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void unhex20(const char *hex, uint8_t out[20]) {
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)((hexv(hex[2*i]) << 4) | hexv(hex[2*i+1]));
}

static const struct pack_obj *find_sha(const struct pack_obj *o, int n, const uint8_t sha[20]) {
    for (int i = 0; i < n; i++) if (memcmp(o[i].sha, sha, 20) == 0) return &o[i];
    return NULL;
}

/* One decoded tree entry. */
struct tent { char mode[8]; char name[256]; uint8_t sha[20]; int isdir; };

/* git's tree-entry order: names compared byte-wise, but a directory sorts as if
 * its name had a trailing '/'. Matters only when one name prefixes another. */
static int tent_cmp(const void *a, const void *b) {
    const struct tent *x = a, *y = b;
    size_t lx = strlen(x->name), ly = strlen(y->name), c = lx < ly ? lx : ly;
    int r = memcmp(x->name, y->name, c);
    if (r) return r;
    int cx = (c < lx) ? (unsigned char)x->name[c] : (x->isdir ? '/' : 0);
    int cy = (c < ly) ? (unsigned char)y->name[c] : (y->isdir ? '/' : 0);
    return cx - cy;
}

/* Splice the tree `tree_sha` (found in base): add/replace the top-level entry
 * `fname` -> (100644, blob_sha), keep the rest, and re-serialize in git order.
 * Sets *out (malloc'd) and *outlen. 0 or -1. */
static int splice_tree(const struct pack_obj *base, int base_n, const uint8_t tree_sha[20],
                       const char *fname, const uint8_t blob_sha[20],
                       uint8_t **out, size_t *outlen) {
    const struct pack_obj *t = find_sha(base, base_n, tree_sha);
    if (!t || t->type != OBJ_TREE) return -1;

    struct tent *e = malloc(sizeof(*e) * 4096);
    if (!e) return -1;
    int n = 0;
    const uint8_t *p = t->data, *end = t->data + t->size;
    while (p < end && n < 4095) {
        const uint8_t *sp = memchr(p, ' ', (size_t)(end - p));
        if (!sp) break;
        size_t ml = (size_t)(sp - p); if (ml >= sizeof e[n].mode) break;
        memcpy(e[n].mode, p, ml); e[n].mode[ml] = 0;
        const uint8_t *nul = memchr(sp + 1, 0, (size_t)(end - (sp + 1)));
        if (!nul) break;
        size_t nl = (size_t)(nul - (sp + 1)); if (nl >= sizeof e[n].name) break;
        memcpy(e[n].name, sp + 1, nl); e[n].name[nl] = 0;
        if (nul + 1 + 20 > end) break;
        memcpy(e[n].sha, nul + 1, 20);
        e[n].isdir = (!strcmp(e[n].mode, "40000") || !strcmp(e[n].mode, "040000"));
        p = nul + 1 + 20;
        n++;
    }

    int replaced = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, fname)) {
            memcpy(e[i].sha, blob_sha, 20); strcpy(e[i].mode, "100644"); e[i].isdir = 0; replaced = 1;
        }
    if (!replaced) {
        if (n >= 4095) { free(e); return -1; }
        strcpy(e[n].mode, "100644");
        strncpy(e[n].name, fname, sizeof e[n].name - 1); e[n].name[sizeof e[n].name - 1] = 0;
        memcpy(e[n].sha, blob_sha, 20); e[n].isdir = 0; n++;
    }
    qsort(e, (size_t)n, sizeof *e, tent_cmp);

    /* serialize: for each entry "<mode> <name>\0" + 20 raw sha */
    size_t cap = 0;
    for (int i = 0; i < n; i++) cap += strlen(e[i].mode) + 1 + strlen(e[i].name) + 1 + 20;
    uint8_t *td = malloc(cap ? cap : 1);
    if (!td) { free(e); return -1; }
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t ml = strlen(e[i].mode), nl = strlen(e[i].name);
        memcpy(td + o, e[i].mode, ml); o += ml;
        td[o++] = ' ';
        memcpy(td + o, e[i].name, nl); o += nl;
        td[o++] = 0;
        memcpy(td + o, e[i].sha, 20); o += 20;
    }
    free(e);
    *out = td; *outlen = o;
    return 0;
}

int push_make_next_commit(const char *fname, const void *content, size_t clen,
                          const char *msg, const char *author, uint32_t ts,
                          const char *parent_hex,
                          const struct pack_obj *base, int base_n,
                          struct pack_obj out[3], char newcommit_hex[41]) {
    struct pack_obj *blob = &out[0], *tree = &out[1], *commit = &out[2];

    /* blob */
    blob->type = OBJ_BLOB; blob->size = clen;
    blob->data = malloc(clen ? clen : 1);
    if (!blob->data) return -1;
    memcpy(blob->data, content, clen);
    obj_sha("blob", blob->data, clen, blob->sha);

    /* find the parent commit + its root tree, then splice in our blob. */
    uint8_t psha[20]; unhex20(parent_hex, psha);
    const struct pack_obj *pc = find_sha(base, base_n, psha);
    if (!pc || pc->type != OBJ_COMMIT || pc->size < 46 ||
        memcmp(pc->data, "tree ", 5) != 0) { free(blob->data); return -1; }
    char ptree_hex[41]; memcpy(ptree_hex, pc->data + 5, 40); ptree_hex[40] = 0;
    uint8_t ptree[20]; unhex20(ptree_hex, ptree);

    uint8_t *td = NULL; size_t tlen = 0;
    if (splice_tree(base, base_n, ptree, fname, blob->sha, &td, &tlen) != 0) { free(blob->data); return -1; }
    tree->type = OBJ_TREE; tree->size = tlen; tree->data = td;
    obj_sha("tree", td, tlen, tree->sha);

    /* commit with a parent line. */
    char thex[41]; hex20(tree->sha, thex);
    char body[1024];
    int bl = snprintf(body, sizeof body,
        "tree %s\n"
        "parent %s\n"
        "author %s %u +0000\n"
        "committer %s %u +0000\n"
        "\n%s\n",
        thex, parent_hex, author, ts, author, ts, msg);
    if (bl < 0 || bl >= (int)sizeof body) { free(blob->data); free(td); return -1; }
    commit->type = OBJ_COMMIT; commit->size = (size_t)bl;
    commit->data = malloc((size_t)bl);
    if (!commit->data) { free(blob->data); free(td); return -1; }
    memcpy(commit->data, body, (size_t)bl);
    obj_sha("commit", commit->data, (size_t)bl, commit->sha);

    hex20(commit->sha, newcommit_hex);
    return 0;
}

int push_make_first_commit(const char *fname, const void *content, size_t clen,
                           const char *msg, const char *author, uint32_t ts,
                           struct pack_obj out[3], char newcommit_hex[41]) {
    struct pack_obj *blob = &out[0], *tree = &out[1], *commit = &out[2];

    /* blob: the file's raw bytes. */
    blob->type = OBJ_BLOB; blob->size = clen;
    blob->data = malloc(clen ? clen : 1);
    if (!blob->data) return -1;
    memcpy(blob->data, content, clen);
    obj_sha("blob", blob->data, clen, blob->sha);

    /* tree: a single regular-file entry "100644 <name>\0" + 20 raw sha bytes. */
    size_t nl = strlen(fname);
    size_t tsize = 7 + nl + 1 + 20;                 /* "100644 " is 7 bytes */
    uint8_t *td = malloc(tsize);
    if (!td) { free(blob->data); return -1; }
    size_t o = 0;
    memcpy(td + o, "100644 ", 7);   o += 7;
    memcpy(td + o, fname, nl);       o += nl;
    td[o++] = 0;
    memcpy(td + o, blob->sha, 20);   o += 20;
    tree->type = OBJ_TREE; tree->size = tsize; tree->data = td;
    obj_sha("tree", td, tsize, tree->sha);

    /* commit: no parent (first commit); author == committer at `ts` UTC. */
    char thex[41]; hex20(tree->sha, thex);
    char body[1024];
    int bl = snprintf(body, sizeof body,
        "tree %s\n"
        "author %s %u +0000\n"
        "committer %s %u +0000\n"
        "\n%s\n",
        thex, author, ts, author, ts, msg);
    if (bl < 0 || bl >= (int)sizeof body) { free(blob->data); free(td); return -1; }
    commit->type = OBJ_COMMIT; commit->size = (size_t)bl;
    commit->data = malloc((size_t)bl);
    if (!commit->data) { free(blob->data); free(td); return -1; }
    memcpy(commit->data, body, (size_t)bl);
    obj_sha("commit", commit->data, (size_t)bl, commit->sha);

    hex20(commit->sha, newcommit_hex);
    return 0;
}

int push_build_request(const char *ref, const char *old_hex, const char *new_hex,
                       const struct pack_obj *objs, int n,
                       uint8_t **req_out, size_t *reqlen) {
    /* command payload: "<old> <new> <ref>\0report-status\n" */
    uint8_t payload[512]; size_t pl = 0;
    int cl = snprintf((char *)payload, sizeof payload, "%s %s %s", old_hex, new_hex, ref);
    if (cl < 0) return -1;
    pl = (size_t)cl;
    payload[pl++] = 0;
    const char *caps = "report-status";
    size_t cn = strlen(caps);
    memcpy(payload + pl, caps, cn); pl += cn;
    payload[pl++] = '\n';

    uint8_t *pack = NULL; size_t packlen = 0;
    if (pack_write(objs, n, &pack, &packlen) != 0) return -1;

    /* pkt-line(command) + flush-pkt + packfile */
    size_t total = (4 + pl) + 4 + packlen;
    uint8_t *req = malloc(total);
    if (!req) { free(pack); return -1; }
    size_t o = 0;
    o += pktline_write(req + o, payload, pl);
    o += pktline_flush(req + o);
    memcpy(req + o, pack, packlen); o += packlen;
    free(pack);

    *req_out = req; *reqlen = o;
    return 0;
}

int push_parse_status(const uint8_t *body, size_t len) {
    /* We negotiated "report-status" (no side-band), so the reply is raw
     * pkt-lines: "unpack ok\n" then "ok <ref>\n" (or "ng <ref> <why>"). */
    const uint8_t *cur = body, *end = body + len, *data; size_t dlen;
    int unpack_ok = 0, ref_ok = 0;
    for (;;) {
        int k = pktline_next(&cur, end, &data, &dlen);
        if (k == PKT_FLUSH) continue;
        if (k != PKT_DATA) break;
        if (dlen >= 9 && !strncmp((const char *)data, "unpack ok", 9)) unpack_ok = 1;
        else if (dlen >= 3 && !strncmp((const char *)data, "ok ", 3))  ref_ok = 1;
        else if (dlen >= 3 && !strncmp((const char *)data, "ng ", 3))  ref_ok = 0;
    }
    return (unpack_ok && ref_ok) ? 0 : -1;
}
