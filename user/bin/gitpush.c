/* gitpush -- push commits to a git repo over HTTPS, our own way.
 *
 * Like gitclone, the stock git can't push here (its HTTPS transport fork/execs
 * git-remote-https + send-pack). So this drives git's smart-HTTP git-receive-pack
 * protocol directly over libtls: it builds a blob+tree(s)+commit, serializes them
 * into a packfile (pack_write) + a ref-update command, POSTs that, and reads the
 * report-status. Authentication is HTTP Basic with a token from the environment.
 *
 * Modes:
 *   - new branch:   the branch doesn't exist -> a first commit (no parent).
 *   - incremental:  the branch exists -> fetch its tip, splice the parent's tree
 *                   (nested paths supported) so other files survive, parent = tip.
 *   - --force:      replace the branch with an unrelated (orphan) commit.
 *   - --delete:     remove the branch (new id = zeros, empty pack).
 *
 * usage: gitpush [--force] <https-url> <local-file> <path-in-repo> [branch]
 *        gitpush --delete <https-url> <branch>
 *   env GITPUSH_TOKEN = "<user>:<token>" or just "<token>" (a GitHub PAT works
 *   as the password with any username). Passed via env, never argv.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "githttp.h"
#include "pktline.h"
#include "pack.h"
#include "push.h"

/* base64 of src -> out (NUL-terminated). out must hold 4*ceil(n/3)+1. */
static void base64(const uint8_t *src, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = src[i] << 16;
        if (i + 1 < n) v |= src[i+1] << 8;
        if (i + 2 < n) v |= src[i+2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* Fetch the objects reachable from `want` (shallow, depth 1) via upload-pack --
 * used to read the parent commit + its tree so an incremental commit can splice
 * it. Sets *objs (caller frees each .data and the array) and *n. 0 or -1. */
static int fetch_tip(const char *base, const char *want, const char *authb64,
                     struct pack_obj **objs, int *n) {
    char url[2048]; snprintf(url, sizeof url, "%s/git-upload-pack", base);
    uint8_t req[192]; size_t rl = 0;
    char wl[64]; int wn = snprintf(wl, sizeof wl, "want %s\n", want);
    rl += pktline_write(req + rl, wl, (size_t)wn);
    rl += pktline_write(req + rl, "deepen 1\n", 9);
    rl += pktline_flush(req + rl);
    rl += pktline_write(req + rl, "done\n", 5);

    uint8_t *body = NULL; size_t len = 0; int status = 0;
    if (git_http("POST", url, "application/x-git-upload-pack-request",
                 req, rl, authb64, &body, &len, &status) != 0) return -1;
    if (status / 100 != 2) { free(body); return -1; }

    /* skip shallow lines + flush, stop at NAK/ACK -- then the raw pack. */
    const uint8_t *cur = body, *end = body + len, *data; size_t dlen;
    for (;;) {
        int k = pktline_next(&cur, end, &data, &dlen);
        if (k == PKT_FLUSH) continue;
        if (k != PKT_DATA) { free(body); return -1; }
        if (dlen >= 8 && !strncmp((const char *)data, "shallow ", 8)) continue;
        if (dlen >= 10 && !strncmp((const char *)data, "unshallow ", 10)) continue;
        if (dlen >= 3 && (!strncmp((const char *)data, "NAK", 3) ||
                          !strncmp((const char *)data, "ACK", 3))) break;
    }
    const uint8_t *pack = cur; size_t plen = (size_t)(end - cur);
    if (plen < 12 || memcmp(pack, "PACK", 4) != 0) { free(body); return -1; }
    int nc = pack_unpack(pack, plen, objs);
    free(body);
    if (nc < 0) return -1;
    *n = nc;
    return 0;
}

/* "<base>.git" from a user URL (strip trailing '/', add .git if absent). */
static void base_git(const char *in, char *out, size_t cap) {
    char base[1600];
    strncpy(base, in, sizeof base - 1); base[sizeof base - 1] = 0;
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') base[--n] = 0;
    int has = (n >= 4 && strcmp(base + n - 4, ".git") == 0);
    snprintf(out, cap, "%s%s", base, has ? "" : ".git");
}

/* Find refs/heads/<branch>'s current id in a receive-pack advertisement, or
 * leave old_hex as 40 zeros (a new branch). */
static void find_ref(const uint8_t *body, size_t len, const char *branch, char old_hex[41]) {
    memset(old_hex, '0', 40); old_hex[40] = 0;
    char want[160]; snprintf(want, sizeof want, "refs/heads/%s", branch);
    size_t wl = strlen(want);

    const uint8_t *cur = body, *end = body + len, *data; size_t dlen;
    int k = pktline_next(&cur, end, &data, &dlen);       /* "# service=..." */
    if (k == PKT_DATA && dlen >= 9 && !strncmp((const char *)data, "# service", 9))
        ;                                                /* fall through to refs */
    for (;;) {
        k = pktline_next(&cur, end, &data, &dlen);
        if (k == PKT_FLUSH) continue;
        if (k != PKT_DATA) break;
        if (dlen < 41 || data[40] != ' ') continue;
        const uint8_t *name = data + 41; size_t nlen = dlen - 41;
        for (size_t i = 0; i < nlen; i++)
            if (name[i] == '\0' || name[i] == '\n') { nlen = i; break; }
        if (nlen == wl && !memcmp(name, want, wl)) { memcpy(old_hex, data, 40); old_hex[40] = 0; return; }
    }
}

int main(int argc, char **argv) {
    /* usage:
     *   gitpush [--force] <url> <local-file> <path-in-repo> [branch]
     *   gitpush --delete <url> <branch>
     * --force replaces the branch with an unrelated (orphan) commit;
     * --delete removes the branch. */
    int do_force = 0, do_delete = 0;
    const char *pos[5]; int np = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--force"))  do_force = 1;
        else if (!strcmp(argv[i], "--delete")) do_delete = 1;
        else if (np < 5) pos[np++] = argv[i];
    }
    const char *urlarg = np > 0 ? pos[0] : NULL;
    const char *localfile = NULL, *repopath = NULL, *branch = "main";
    if (do_delete) { branch = np > 1 ? pos[1] : "main"; }
    else { localfile = np > 1 ? pos[1] : NULL; repopath = np > 2 ? pos[2] : NULL;
           branch = np > 3 ? pos[3] : "main"; }
    if (!urlarg || (!do_delete && (!localfile || !repopath))) {
        fprintf(stderr, "usage: gitpush [--force] <url> <file> <path> [branch]\n"
                        "       gitpush --delete <url> <branch>\n");
        return 2;
    }

    const char *token = getenv("GITPUSH_TOKEN");
    if (!token || !*token) { fprintf(stderr, "gitpush: set GITPUSH_TOKEN in the environment\n"); return 2; }
    char userpass[512];
    if (strchr(token, ':')) snprintf(userpass, sizeof userpass, "%s", token);      /* user:token */
    else                    snprintf(userpass, sizeof userpass, "git:%s", token);  /* PAT as password */
    char authb64[720];
    base64((const uint8_t *)userpass, strlen(userpass), authb64);

    /* Read the file to push (skipped for --delete). */
    long fl = 0; uint8_t *content = NULL;
    if (!do_delete) {
        FILE *f = fopen(localfile, "rb");
        if (!f) { fprintf(stderr, "gitpush: cannot open %s\n", localfile); return 1; }
        fseek(f, 0, SEEK_END); fl = ftell(f); fseek(f, 0, SEEK_SET);
        if (fl < 0) { fclose(f); return 1; }
        content = malloc((size_t)fl ? (size_t)fl : 1);
        if (fread(content, 1, (size_t)fl, f) != (size_t)fl) { fclose(f); free(content); return 1; }
        fclose(f);
    }

    char base[2048]; base_git(urlarg, base, sizeof base);

    /* Discover the branch's current id (zeros => the branch doesn't exist). */
    char url[2200]; snprintf(url, sizeof url, "%s/info/refs?service=git-receive-pack", base);
    uint8_t *body = NULL; size_t len = 0; int status = 0;
    if (git_http("GET", url, NULL, NULL, 0, authb64, &body, &len, &status) != 0) {
        fprintf(stderr, "gitpush: transport failed for %s\n", url); free(content); return 1;
    }
    if (status == 401 || status == 403) { fprintf(stderr, "gitpush: auth rejected (HTTP %d)\n", status); free(body); free(content); return 1; }
    if (status / 100 != 2) { fprintf(stderr, "gitpush: HTTP %d discovering refs\n", status); free(body); free(content); return 1; }
    char old_hex[41]; find_ref(body, len, branch, old_hex);
    free(body);
    int exists = (strspn(old_hex, "0") != 40);
    char ref[160]; snprintf(ref, sizeof ref, "refs/heads/%s", branch);

    uint8_t *req = NULL; size_t reqlen = 0;
    char new_hex[41] = "0000000000000000000000000000000000000000";

    if (do_delete) {
        if (!exists) { fprintf(stderr, "gitpush: %s does not exist -- nothing to delete\n", branch); return 1; }
        printf("GITPUSH %s: deleting %s (%s)\n", urlarg, branch, old_hex);
        if (push_build_delete(ref, old_hex, &req, &reqlen) != 0) {
            fprintf(stderr, "gitpush: serializing the delete failed\n"); return 1;
        }
    } else {
        printf("GITPUSH %s: %s currently %s (%s)\n", urlarg, branch, old_hex,
               !exists ? "new branch" : do_force ? "FORCE" : "incremental");
        /* First commit / force => no parent (fresh trees). Incremental => fetch
         * the tip and splice its tree so existing files survive. */
        int use_parent = (exists && !do_force);
        const char *msg = !exists ? "first commit pushed from EmbLinkOS"
                        : do_force ? "force-pushed from EmbLinkOS (history replaced)"
                                   : "another commit pushed from EmbLinkOS";
        struct pack_obj *objs = NULL; int nobjs = 0; int build_rc;
        if (use_parent) {
            struct pack_obj *tip = NULL; int tn = 0;
            if (fetch_tip(base, old_hex, authb64, &tip, &tn) != 0) {
                fprintf(stderr, "gitpush: fetching the parent tip failed\n"); free(content); return 1;
            }
            build_rc = push_make_commit(repopath, content, (size_t)fl, msg,
                                        "EmbLink <os@emblink>", (uint32_t)time(NULL),
                                        old_hex, tip, tn, &objs, &nobjs, new_hex);
            for (int i = 0; i < tn; i++) free(tip[i].data);
            free(tip);
        } else {
            build_rc = push_make_commit(repopath, content, (size_t)fl, msg,
                                        "EmbLink <os@emblink>", (uint32_t)time(NULL),
                                        NULL, NULL, 0, &objs, &nobjs, new_hex);
        }
        if (build_rc != 0) { fprintf(stderr, "gitpush: building the commit failed\n"); free(content); return 1; }
        free(content);
        if (push_build_request(ref, old_hex, new_hex, objs, nobjs, &req, &reqlen) != 0) {
            fprintf(stderr, "gitpush: serializing the request failed\n");
            for (int i = 0; i < nobjs; i++) free(objs[i].data); free(objs); return 1;
        }
        for (int i = 0; i < nobjs; i++) free(objs[i].data); free(objs);
        printf("GITPUSH new commit %s (%d objects, %zu-byte request)\n", new_hex, nobjs, reqlen);
    }

    /* POST it. */
    snprintf(url, sizeof url, "%s/git-receive-pack", base);
    body = NULL; len = 0; status = 0;
    int rc = git_http("POST", url, "application/x-git-receive-pack-request",
                      req, reqlen, authb64, &body, &len, &status);
    free(req);
    if (rc != 0) { fprintf(stderr, "gitpush: receive-pack transport failed\n"); return 1; }
    if (status == 401 || status == 403) { fprintf(stderr, "gitpush: auth rejected on push (HTTP %d)\n", status); free(body); return 1; }
    if (status / 100 != 2) { fprintf(stderr, "gitpush: HTTP %d from receive-pack\n", status); free(body); return 1; }

    int ok = (push_parse_status(body, len) == 0);
    free(body);
    printf("GITPUSH %s %s -> %s\n", ref, new_hex, ok ? "OK" : "REJECTED");
    return ok ? 0 : 1;
}
