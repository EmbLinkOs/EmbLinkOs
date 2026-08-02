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
