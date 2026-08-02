/* git push core -- see push.h. Transport-independent: object construction
 * (blob + the trees along a path + commit, splicing an existing tree so other
 * entries survive), request serialization, and report-status parsing. */
#include "push.h"
#include "pktline.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SHA-1 of a git object: "<type> <size>\0" + content. */
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

/* A growable list of built objects (the caller frees each .data and the array). */
struct objlist { struct pack_obj *o; int n, cap; };
static int ol_add(struct objlist *L, int type, uint8_t *data, size_t size, uint8_t sha_out[20]) {
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 8;
        struct pack_obj *no = realloc(L->o, (size_t)nc * sizeof *no);
        if (!no) return -1;
        L->o = no; L->cap = nc;
    }
    struct pack_obj *e = &L->o[L->n++];
    e->type = type; e->data = data; e->size = size;
    obj_sha(pack_type_name(type), data, size, e->sha);
    if (sha_out) memcpy(sha_out, e->sha, 20);
    return 0;
}
static void ol_free(struct objlist *L) {
    for (int i = 0; i < L->n; i++) free(L->o[i].data);
    free(L->o);
}

/* One decoded tree entry. */
struct tent { char mode[8]; char name[256]; uint8_t sha[20]; int isdir; };

/* git tree order: names byte-wise, but a directory sorts as if name had '/'. */
static int tent_cmp(const void *a, const void *b) {
    const struct tent *x = a, *y = b;
    size_t lx = strlen(x->name), ly = strlen(y->name), c = lx < ly ? lx : ly;
    int r = memcmp(x->name, y->name, c);
    if (r) return r;
    int cx = (c < lx) ? (unsigned char)x->name[c] : (x->isdir ? '/' : 0);
    int cy = (c < ly) ? (unsigned char)y->name[c] : (y->isdir ? '/' : 0);
    return cx - cy;
}

/* Parse a tree object's entries into e[0..*n). Returns 0 or -1. */
static int parse_tree(const struct pack_obj *t, struct tent *e, int *np) {
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
    *np = n;
    return 0;
}

/* Build the (spliced) tree for path components comp[0..ncomp): set comp[0] to
 * either the leaf blob (ncomp==1) or a recursively-spliced subtree (ncomp>1),
 * preserving every other entry of `base_tree_sha` (NULL => a fresh empty tree).
 * Appends each new tree object to `out` and returns this level's tree sha. */
static int splice_path(const struct pack_obj *base, int base_n,
                       const uint8_t *base_tree_sha,
                       char **comp, int ncomp, const uint8_t leaf_blob_sha[20],
                       struct objlist *out, uint8_t new_tree_sha[20]) {
    struct tent *e = malloc(sizeof *e * 4096);
    if (!e) return -1;
    int n = 0;
    if (base_tree_sha) {
        const struct pack_obj *t = find_sha(base, base_n, base_tree_sha);
        if (!t || t->type != OBJ_TREE) { free(e); return -1; }
        if (parse_tree(t, e, &n) != 0) { free(e); return -1; }
    }

    uint8_t child_sha[20]; char child_mode[8]; int child_isdir;
    if (ncomp == 1) {                                    /* the file itself */
        memcpy(child_sha, leaf_blob_sha, 20);
        strcpy(child_mode, "100644"); child_isdir = 0;
    } else {                                             /* descend into a subdir */
        const uint8_t *sub = NULL;
        for (int i = 0; i < n; i++)
            if (e[i].isdir && !strcmp(e[i].name, comp[0])) { sub = e[i].sha; break; }
        if (splice_path(base, base_n, sub, comp + 1, ncomp - 1, leaf_blob_sha, out, child_sha) != 0) {
            free(e); return -1;
        }
        strcpy(child_mode, "40000"); child_isdir = 1;
    }

    int replaced = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, comp[0])) {
            memcpy(e[i].sha, child_sha, 20); strcpy(e[i].mode, child_mode);
            e[i].isdir = child_isdir; replaced = 1;
        }
    if (!replaced) {
        if (n >= 4095) { free(e); return -1; }
        strcpy(e[n].mode, child_mode);
        strncpy(e[n].name, comp[0], sizeof e[n].name - 1); e[n].name[sizeof e[n].name - 1] = 0;
        memcpy(e[n].sha, child_sha, 20); e[n].isdir = child_isdir; n++;
    }
    qsort(e, (size_t)n, sizeof *e, tent_cmp);

    size_t cap = 0;
    for (int i = 0; i < n; i++) cap += strlen(e[i].mode) + 1 + strlen(e[i].name) + 1 + 20;
    uint8_t *td = malloc(cap ? cap : 1);
    if (!td) { free(e); return -1; }
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t ml = strlen(e[i].mode), nl = strlen(e[i].name);
        memcpy(td + o, e[i].mode, ml); o += ml; td[o++] = ' ';
        memcpy(td + o, e[i].name, nl); o += nl; td[o++] = 0;
        memcpy(td + o, e[i].sha, 20); o += 20;
    }
    free(e);
    if (ol_add(out, OBJ_TREE, td, o, new_tree_sha) != 0) { free(td); return -1; }
    return 0;
}

int push_make_commit(const char *path, const void *content, size_t clen,
                     const char *msg, const char *author, uint32_t ts,
                     const char *parent_hex, const struct pack_obj *base, int base_n,
                     struct pack_obj **out, int *nout, char newcommit_hex[41]) {
    struct objlist L = {0};

    /* blob */
    uint8_t *bd = malloc(clen ? clen : 1);
    if (!bd) return -1;
    memcpy(bd, content, clen);
    uint8_t blob_sha[20];
    if (ol_add(&L, OBJ_BLOB, bd, clen, blob_sha) != 0) { free(bd); return -1; }

    /* split the path into components */
    char pbuf[1024]; strncpy(pbuf, path, sizeof pbuf - 1); pbuf[sizeof pbuf - 1] = 0;
    char *comp[64]; int ncomp = 0;
    for (char *tok = strtok(pbuf, "/"); tok && ncomp < 64; tok = strtok(NULL, "/")) comp[ncomp++] = tok;
    if (ncomp == 0) { ol_free(&L); return -1; }

    /* base root tree (incremental) or none (first commit) */
    uint8_t rootbase[20]; const uint8_t *root_base = NULL;
    if (parent_hex) {
        uint8_t psha[20]; unhex20(parent_hex, psha);
        const struct pack_obj *pc = find_sha(base, base_n, psha);
        if (!pc || pc->type != OBJ_COMMIT || pc->size < 46 || memcmp(pc->data, "tree ", 5)) { ol_free(&L); return -1; }
        char thex[41]; memcpy(thex, pc->data + 5, 40); thex[40] = 0;
        unhex20(thex, rootbase); root_base = rootbase;
    }

    uint8_t root_sha[20];
    if (splice_path(base, base_n, root_base, comp, ncomp, blob_sha, &L, root_sha) != 0) { ol_free(&L); return -1; }

    /* commit (with a parent line for an incremental commit) */
    char thex[41]; hex20(root_sha, thex);
    char body[1024]; int bl;
    if (parent_hex)
        bl = snprintf(body, sizeof body,
            "tree %s\nparent %s\nauthor %s %u +0000\ncommitter %s %u +0000\n\n%s\n",
            thex, parent_hex, author, ts, author, ts, msg);
    else
        bl = snprintf(body, sizeof body,
            "tree %s\nauthor %s %u +0000\ncommitter %s %u +0000\n\n%s\n",
            thex, author, ts, author, ts, msg);
    if (bl < 0 || bl >= (int)sizeof body) { ol_free(&L); return -1; }
    uint8_t *cd = malloc((size_t)bl);
    if (!cd) { ol_free(&L); return -1; }
    memcpy(cd, body, (size_t)bl);
    uint8_t csha[20];
    if (ol_add(&L, OBJ_COMMIT, cd, (size_t)bl, csha) != 0) { free(cd); ol_free(&L); return -1; }
    hex20(csha, newcommit_hex);

    *out = L.o; *nout = L.n;
    return 0;
}

/* Assemble pkt-line(command) + flush-pkt + packfile. */
static int build_req(const char *cmd_payload, size_t plen,
                     const struct pack_obj *objs, int n,
                     uint8_t **req_out, size_t *reqlen) {
    uint8_t *pack = NULL; size_t packlen = 0;
    if (pack_write(objs, n, &pack, &packlen) != 0) return -1;
    size_t total = (4 + plen) + 4 + packlen;
    uint8_t *req = malloc(total);
    if (!req) { free(pack); return -1; }
    size_t o = 0;
    o += pktline_write(req + o, cmd_payload, plen);
    o += pktline_flush(req + o);
    memcpy(req + o, pack, packlen); o += packlen;
    free(pack);
    *req_out = req; *reqlen = o;
    return 0;
}

int push_build_request(const char *ref, const char *old_hex, const char *new_hex,
                       const struct pack_obj *objs, int n,
                       uint8_t **req_out, size_t *reqlen) {
    uint8_t payload[512]; size_t pl = 0;
    int cl = snprintf((char *)payload, sizeof payload, "%s %s %s", old_hex, new_hex, ref);
    if (cl < 0) return -1;
    pl = (size_t)cl; payload[pl++] = 0;
    const char *caps = "report-status"; size_t cn = strlen(caps);
    memcpy(payload + pl, caps, cn); pl += cn; payload[pl++] = '\n';
    return build_req((const char *)payload, pl, objs, n, req_out, reqlen);
}

int push_build_delete(const char *ref, const char *old_hex,
                      uint8_t **req_out, size_t *reqlen) {
    /* Delete = update to the zero id, with an EMPTY packfile (0 objects). */
    char zeros[41]; memset(zeros, '0', 40); zeros[40] = 0;
    uint8_t payload[512]; size_t pl = 0;
    int cl = snprintf((char *)payload, sizeof payload, "%s %s %s", old_hex, zeros, ref);
    if (cl < 0) return -1;
    pl = (size_t)cl; payload[pl++] = 0;
    const char *caps = "report-status"; size_t cn = strlen(caps);
    memcpy(payload + pl, caps, cn); pl += cn; payload[pl++] = '\n';
    return build_req((const char *)payload, pl, NULL, 0, req_out, reqlen);
}

int push_parse_status(const uint8_t *body, size_t len) {
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
