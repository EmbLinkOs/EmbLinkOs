/* gitclone -- clone a git repo over HTTPS, our own way.
 *
 * The stock git can't do this here: its HTTPS transport fork/execs
 * git-remote-https and index-pack, and EmbLink has no fork/exec. So this tool
 * drives git's smart-HTTP protocol directly over libtls (docs/TLS.md) and writes
 * a real .git the on-OS git can then operate on.
 *
 * Milestones (user/git/):
 *   G1  ref discovery -- GET info/refs, list refs.                    [done]
 *   G2  fetch         -- POST git-upload-pack with the wants,          [done]
 *                        receive the packfile.
 *   G3  unpack        -- packfile -> objects (SHA1 + inflate + delta). [done]
 *   G4  refs+checkout -- write a real .git + working tree to disk.     [done]
 *
 * usage: gitclone <https-url> [dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "githttp.h"
#include "pktline.h"
#include "pack.h"
#include "sha1.h"
#include "repo.h"

/* "<base>.git" from a user URL (strip trailing '/', add .git if absent). */
static void base_git(const char *in, char *out, size_t cap) {
    char base[1600];
    strncpy(base, in, sizeof base - 1); base[sizeof base - 1] = 0;
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') base[--n] = 0;
    int has_git = (n >= 4 && strcmp(base + n - 4, ".git") == 0);
    snprintf(out, cap, "%s%s", base, has_git ? "" : ".git");
}

/* Parse a smart-HTTP ref advertisement (pkt-line). Prints a short summary,
 * counts refs, and copies HEAD's object id (the first ref) into want[41].
 * Returns the ref count, or -1 on a malformed advertisement. */
static int read_refs(const uint8_t *body, size_t len, char want[41], char branch[128]) {
    const uint8_t *cur = body, *end = body + len, *data; size_t dlen;
    want[0] = 0; strcpy(branch, "master");           /* default if none matches */

    int k = pktline_next(&cur, end, &data, &dlen);      /* "# service=..." then flush */
    if (k == PKT_DATA && dlen >= 9 && !strncmp((const char *)data, "# service", 9))
        k = pktline_next(&cur, end, &data, &dlen);

    int count = 0;
    for (;;) {
        k = pktline_next(&cur, end, &data, &dlen);
        if (k == PKT_FLUSH) break;
        if (k != PKT_DATA) { if (count == 0) return -1; break; }
        if (dlen < 41 || data[40] != ' ') continue;

        if (!want[0]) { memcpy(want, data, 40); want[40] = 0; }   /* first ref = HEAD */

        const uint8_t *name = data + 41; size_t nlen = dlen - 41;
        for (size_t i = 0; i < nlen; i++)
            if (name[i] == '\0' || name[i] == '\n') { nlen = i; break; }

        /* The default branch: a refs/heads/<X> pointing at HEAD's object id. */
        if (nlen > 11 && !memcmp(name, "refs/heads/", 11) && !memcmp(data, want, 40)) {
            size_t bl = nlen - 11; if (bl > 126) bl = 126;
            memcpy(branch, name + 11, bl); branch[bl] = 0;
        }
        if (count < 3) { char sha[41]; memcpy(sha, data, 40); sha[40] = 0;
            printf("  %s %.*s\n", sha, (int)nlen, name); }
        count++;
    }
    return count;
}

/* G2: POST git-upload-pack with a single want (a full clone of HEAD, no haves)
 * and return the packfile bytes. No side-band capability, so the response is
 * simply "NAK\n" followed by the raw packfile. On success sets pack and packlen
 * (pointing into body, which the caller frees) and returns 0. */
static int fetch_pack(const char *base, const char *want,
                      uint8_t **body_out, const uint8_t **pack, size_t *packlen) {
    char url[2048];
    snprintf(url, sizeof url, "%s/git-upload-pack", base);

    uint8_t req[128]; size_t rl = 0;
    char wl[64]; int wn = snprintf(wl, sizeof wl, "want %s\n", want);
    rl += pktline_write(req + rl, wl, (size_t)wn);
    rl += pktline_flush(req + rl);
    rl += pktline_write(req + rl, "done\n", 5);

    uint8_t *body = NULL; size_t len = 0; int status = 0;
    if (git_http("POST", url, "application/x-git-upload-pack-request",
                 req, rl, &body, &len, &status) != 0) {
        fprintf(stderr, "gitclone: upload-pack transport failed\n");
        return -1;
    }
    if (status / 100 != 2) {
        fprintf(stderr, "gitclone: HTTP %d from upload-pack\n", status);
        free(body); return -1;
    }

    /* Response: one pkt-line ("NAK\n"), then the raw packfile. */
    const uint8_t *cur = body, *end = body + len, *data; size_t dlen;
    int k = pktline_next(&cur, end, &data, &dlen);
    if (k != PKT_DATA || dlen < 3 || strncmp((const char *)data, "NAK", 3) != 0) {
        fprintf(stderr, "gitclone: expected NAK, got a %d-packet\n", k);
        free(body); return -1;
    }
    *pack = cur;
    *packlen = (size_t)(end - cur);
    *body_out = body;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: gitclone <https-url> [dir]\n"); return 2; }

    char base[2048]; base_git(argv[1], base, sizeof base);
    char url[2200]; snprintf(url, sizeof url, "%s/info/refs?service=git-upload-pack", base);

    /* G1: ref discovery. */
    uint8_t *body = NULL; size_t len = 0; int status = 0;
    if (git_http("GET", url, NULL, NULL, 0, &body, &len, &status) != 0) {
        fprintf(stderr, "gitclone: transport failed for %s\n", url); return 1;
    }
    if (status / 100 != 2) { fprintf(stderr, "gitclone: HTTP %d fetching refs\n", status); free(body); return 1; }

    printf("GITCLONE refs of %s:\n", argv[1]);
    char want[41], branch[128];
    int n = read_refs(body, len, want, branch);
    free(body);
    if (n < 0 || !want[0]) { fprintf(stderr, "gitclone: malformed ref advertisement\n"); return 1; }
    printf("GITCLONE %d refs, HEAD=%s (%s)\n", n, want, branch);

    /* G2: fetch the packfile for HEAD. */
    uint8_t *pbody = NULL; const uint8_t *pack = NULL; size_t plen = 0;
    if (fetch_pack(base, want, &pbody, &pack, &plen) != 0) return 1;

    if (plen < 12 || memcmp(pack, "PACK", 4) != 0) {
        fprintf(stderr, "gitclone: not a packfile (%zu bytes)\n", plen);
        free(pbody); return 1;
    }
    unsigned long nobj = ((unsigned long)pack[8] << 24) | (pack[9] << 16) | (pack[10] << 8) | pack[11];
    printf("GITFETCH pack %lu objects, %zu bytes\n", nobj, plen);

    /* G3: unpack the packfile into real objects (inflate + resolve deltas +
     * SHA-1 name each). Verifying the HEAD commit's id matches `want` proves the
     * whole pipe: inflate, delta resolution, and object naming are all correct. */
    struct pack_obj *objs = NULL;
    int nc = pack_unpack(pack, plen, &objs);
    free(pbody);
    if (nc < 0) { fprintf(stderr, "gitclone: packfile unpack failed\n"); return 1; }

    int commits = 0, trees = 0, blobs = 0, tags = 0, head_ok = 0;
    for (int i = 0; i < nc; i++) {
        switch (objs[i].type) {
        case OBJ_COMMIT: commits++; break;
        case OBJ_TREE:   trees++;   break;
        case OBJ_BLOB:   blobs++;   break;
        case OBJ_TAG:    tags++;    break;
        }
        char hex[41]; sha1_hex(objs[i].sha, hex);
        if (strcmp(hex, want) == 0) head_ok = 1;
    }
    printf("GITUNPACK %d objects (%d commit, %d tree, %d blob, %d tag); HEAD %s\n",
           nc, commits, trees, blobs, tags, head_ok ? "reconstructed" : "MISSING");
    if (!head_ok) { fprintf(stderr, "gitclone: HEAD commit not in pack\n");
                    for (int i = 0; i < nc; i++) free(objs[i].data); free(objs); return 1; }

    /* G4: write a real .git and check out the working tree (if a dir is given). */
    int rc = 0;
    if (argc >= 3) {
        const char *dir = argv[2];
        int ow = 0, files = 0;
        if (repo_init(dir) != 0 ||
            (ow = repo_write_objects(dir, objs, nc)) < 0 ||
            repo_write_refs(dir, branch, want) != 0 ||
            (files = repo_checkout(dir, objs, nc, want)) < 0) {
            fprintf(stderr, "gitclone: writing repo to %s failed\n", dir);
            rc = 1;
        } else {
            printf("GITCHECKOUT %s: %d objects, %d files, branch %s -> OK\n", dir, ow, files, branch);
        }
    } else {
        printf("GITUNPACK -> OK (no dir; not checked out)\n");
    }

    for (int i = 0; i < nc; i++) free(objs[i].data);
    free(objs);
    return rc;
}
