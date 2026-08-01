/* gitclone -- clone a git repo over HTTPS, our own way.
 *
 * The stock git can't do this here: its HTTPS transport fork/execs
 * git-remote-https and index-pack, and EmbLink has no fork/exec. So this tool
 * drives git's smart-HTTP protocol directly over libtls (docs/TLS.md) and writes
 * a real .git the on-OS git can then operate on.
 *
 * Milestone G1 (this file today): ref discovery -- GET info/refs and list the
 * remote's refs, proving the git-protocol-over-TLS path. Fetch (POST
 * upload-pack), packfile unpack, and checkout come next (see user/git/).
 *
 * usage: gitclone <https-url> [dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "githttp.h"
#include "pktline.h"

/* Build "<base>.git/info/refs?service=git-upload-pack" from a user URL. */
static void refs_url(const char *in, char *out, size_t cap) {
    char base[1600];
    strncpy(base, in, sizeof base - 1); base[sizeof base - 1] = 0;
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') base[--n] = 0;     /* strip trailing / */
    int has_git = (n >= 4 && strcmp(base + n - 4, ".git") == 0);
    snprintf(out, cap, "%s%s/info/refs?service=git-upload-pack",
             base, has_git ? "" : ".git");
}

/* List the refs in a smart-HTTP ref advertisement (pkt-line). Returns the count,
 * or -1 on a malformed advertisement. */
static int list_refs(const uint8_t *body, size_t len) {
    const uint8_t *cur = body, *end = body + len;
    const uint8_t *data; size_t dlen;

    /* First packet is "# service=git-upload-pack\n", then a flush. Skip both. */
    int k = pktline_next(&cur, end, &data, &dlen);
    if (k == PKT_DATA && dlen >= 9 && !strncmp((const char *)data, "# service", 9)) {
        k = pktline_next(&cur, end, &data, &dlen);   /* the flush after it */
    }

    int count = 0;
    for (;;) {
        k = pktline_next(&cur, end, &data, &dlen);
        if (k == PKT_FLUSH) break;
        if (k != PKT_DATA) { if (count == 0) return -1; break; }

        /* "<40-hex sha> <refname>[\0capabilities]\n" */
        if (dlen < 41 || data[40] != ' ') continue;
        char sha[41]; memcpy(sha, data, 40); sha[40] = 0;
        const uint8_t *name = data + 41;
        size_t nlen = dlen - 41;
        for (size_t i = 0; i < nlen; i++)          /* name ends at NUL or newline */
            if (name[i] == '\0' || name[i] == '\n') { nlen = i; break; }
        printf("  %s %.*s\n", sha, (int)nlen, name);
        count++;
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gitclone <https-url> [dir]\n");
        return 2;
    }
    char url[2048];
    refs_url(argv[1], url, sizeof url);

    uint8_t *body = NULL; size_t len = 0; int status = 0;
    if (git_http("GET", url, NULL, NULL, 0, &body, &len, &status) != 0) {
        fprintf(stderr, "gitclone: transport failed for %s\n", url);
        return 1;
    }
    if (status / 100 != 2) {
        fprintf(stderr, "gitclone: HTTP %d fetching refs\n", status);
        free(body);
        return 1;
    }

    printf("GITCLONE refs of %s:\n", argv[1]);
    int n = list_refs(body, len);
    free(body);
    if (n < 0) { fprintf(stderr, "gitclone: malformed ref advertisement\n"); return 1; }
    printf("GITCLONE %d refs -> OK\n", n);
    return 0;
}
