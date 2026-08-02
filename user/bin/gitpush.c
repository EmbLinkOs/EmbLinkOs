/* gitpush -- push a first commit to a git repo over HTTPS, our own way.
 *
 * Like gitclone, the stock git can't push here (its HTTPS transport fork/execs
 * git-remote-https + send-pack). So this drives git's smart-HTTP git-receive-pack
 * protocol directly over libtls: it builds a blob+tree+commit, serializes them
 * into a packfile (pack_write) + a ref-update command, POSTs that, and reads the
 * report-status. Authentication is HTTP Basic with a token from the environment.
 *
 * Scope: pushes a SINGLE first commit (one file, no parent) to CREATE a branch,
 * i.e. a push to an empty repo / brand-new branch. That keeps the tree trivial
 * (no need to read+splice the parent's tree). Enough to prove the push pipe.
 *
 * usage: gitpush <https-url> <local-file> <path-in-repo> [branch]
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
    if (argc < 4) { fprintf(stderr, "usage: gitpush <https-url> <local-file> <path-in-repo> [branch]\n"); return 2; }
    const char *urlarg = argv[1], *localfile = argv[2], *repopath = argv[3];
    const char *branch = argc >= 5 ? argv[4] : "main";

    const char *token = getenv("GITPUSH_TOKEN");
    if (!token || !*token) { fprintf(stderr, "gitpush: set GITPUSH_TOKEN in the environment\n"); return 2; }
    char userpass[512];
    if (strchr(token, ':')) snprintf(userpass, sizeof userpass, "%s", token);      /* user:token */
    else                    snprintf(userpass, sizeof userpass, "git:%s", token);  /* PAT as password */
    char authb64[720];
    base64((const uint8_t *)userpass, strlen(userpass), authb64);

    /* Read the file to push. */
    FILE *f = fopen(localfile, "rb");
    if (!f) { fprintf(stderr, "gitpush: cannot open %s\n", localfile); return 1; }
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    if (fl < 0) { fclose(f); return 1; }
    uint8_t *content = malloc((size_t)fl ? (size_t)fl : 1);
    if (fread(content, 1, (size_t)fl, f) != (size_t)fl) { fclose(f); free(content); return 1; }
    fclose(f);

    char base[2048]; base_git(urlarg, base, sizeof base);

    /* Discover the branch's current id (zeros => create). */
    char url[2200]; snprintf(url, sizeof url, "%s/info/refs?service=git-receive-pack", base);
    uint8_t *body = NULL; size_t len = 0; int status = 0;
    if (git_http("GET", url, NULL, NULL, 0, authb64, &body, &len, &status) != 0) {
        fprintf(stderr, "gitpush: transport failed for %s\n", url); free(content); return 1;
    }
    if (status == 401 || status == 403) { fprintf(stderr, "gitpush: auth rejected (HTTP %d)\n", status); free(body); free(content); return 1; }
    if (status / 100 != 2) { fprintf(stderr, "gitpush: HTTP %d discovering refs\n", status); free(body); free(content); return 1; }
    char old_hex[41]; find_ref(body, len, branch, old_hex);
    free(body);
    printf("GITPUSH %s: %s currently %s\n", urlarg, branch, old_hex);

    /* Build the first commit + the receive-pack request. */
    struct pack_obj objs[3]; char new_hex[41];
    if (push_make_first_commit(repopath, content, (size_t)fl,
                               "first commit pushed from EmbLinkOS",
                               "EmbLink <os@emblink>", (uint32_t)time(NULL),
                               objs, new_hex) != 0) {
        fprintf(stderr, "gitpush: building the commit failed\n"); free(content); return 1;
    }
    free(content);
    char ref[160]; snprintf(ref, sizeof ref, "refs/heads/%s", branch);
    uint8_t *req = NULL; size_t reqlen = 0;
    if (push_build_request(ref, old_hex, new_hex, objs, 3, &req, &reqlen) != 0) {
        fprintf(stderr, "gitpush: serializing the request failed\n");
        for (int i = 0; i < 3; i++) free(objs[i].data);
        return 1;
    }
    for (int i = 0; i < 3; i++) free(objs[i].data);
    printf("GITPUSH new commit %s (%zu-byte receive-pack request)\n", new_hex, reqlen);

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
