/* Git smart-HTTP transport over libtls -- see githttp.h. Adapted from the
 * pkgfetch HTTPS helper (user/bin/pkgfetch.c), generalized to GET+POST and to
 * return the raw body (git speaks its own pkt-line framing on top). */
#include "githttp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>            /* getaddrinfo -- POSIX resolve+connect */
#include "tls.h"              /* libtls: tls_connect/read/write/close */

#define GITHTTP_MAX_BODY (16u * 1024 * 1024)   /* cap a single response at 16 MB */

static int split_url(const char *url, char *host, int hostsz,
                     char *path, int pathsz) {
    if (strncmp(url, "https://", 8) != 0) return -1;
    const char *p = url + 8;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && i < hostsz - 1) host[i++] = *p++;
    host[i] = 0;
    while (*p && *p != '/') p++;                 /* skip any :port */
    if (*p == '/') { strncpy(path, p, pathsz - 1); path[pathsz - 1] = 0; }
    else strcpy(path, "/");
    return 0;
}

int git_http(const char *method, const char *url, const char *ctype,
             const uint8_t *req, size_t reqlen, const char *authb64,
             uint8_t **body, size_t *len, int *status) {
    static int depth = 0;
    char host[256], path[2048];
    if (split_url(url, host, sizeof host, path, sizeof path) != 0) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "443", &hints, &res) != 0 || !res) {
        fprintf(stderr, "git: resolve %s failed\n", host);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { freeaddrinfo(res); close(fd); return -1; }
    freeaddrinfo(res);

    struct tls_conn *c = malloc(sizeof *c);
    if (!c) { close(fd); return -1; }
    fprintf(stderr, "githttp: %s %s (TLS handshake)\n", method, host);
    int trc = tls_connect(c, fd, host);
    if (trc != 0) {
        fprintf(stderr, "git: TLS to %s failed (rc=%d)\n", host, trc);
        free(c); close(fd); return -1;
    }
    fprintf(stderr, "githttp: TLS up, sending request\n");

    /* Request headers. HTTP/1.0 + Connection: close => body ends at EOF, so we
     * never have to parse chunked/Content-Length. git-upload-pack tolerates
     * this. Advertise protocol v0 (no Git-Protocol header). */
    char authln[512]; authln[0] = 0;
    if (authb64) snprintf(authln, sizeof authln, "Authorization: Basic %s\r\n", authb64);
    char hdr[1024];
    int hl;
    if (req && reqlen) {
        hl = snprintf(hdr, sizeof hdr,
            "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: git/EmbLinkOS\r\n"
            "Accept: */*\r\n%sContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            method, path, host, authln,
            ctype ? ctype : "application/octet-stream", reqlen);
    } else {
        hl = snprintf(hdr, sizeof hdr,
            "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: git/EmbLinkOS\r\n"
            "Accept: */*\r\n%sConnection: close\r\n\r\n",
            method, path, host, authln);
    }
    if (tls_write(c, hdr, (size_t)hl) < 0) { tls_close(c); free(c); return -1; }
    if (req && reqlen && tls_write(c, req, reqlen) < 0) { tls_close(c); free(c); return -1; }

    uint8_t *buf = malloc(GITHTTP_MAX_BODY);
    if (!buf) { tls_close(c); free(c); return -1; }
    size_t total = 0, next_mark = 512u * 1024;
    for (;;) {
        long n = tls_read(c, buf + total, GITHTTP_MAX_BODY - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= next_mark) {      /* coarse progress: big ref advertisements
                                        * are slow to stream over TLS under TCG */
            fprintf(stderr, "githttp: recv %zu KB\n", total / 1024);
            next_mark += 512u * 1024;
        }
        if (total >= GITHTTP_MAX_BODY) break;
    }
    fprintf(stderr, "githttp: response complete, %zu bytes\n", total);
    tls_close(c); free(c);

    /* Split header/body at CRLFCRLF; read the status line. */
    uint8_t *hdrend = NULL;
    for (size_t i = 0; i + 3 < total; i++)
        if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') { hdrend = buf + i + 4; break; }
    if (!hdrend) { free(buf); return -1; }
    int st = 0;
    if (!strncmp((char *)buf, "HTTP/1.", 7)) st = atoi((char *)buf + 9);
    *status = st;

    if (st >= 300 && st < 400 && depth < 3) {   /* one-hop redirect (GET only) */
        char loc[2048] = {0};
        for (char *q = (char *)buf; q < (char *)hdrend; q++) {
            if ((q == (char *)buf || q[-1] == '\n') && !strncasecmp(q, "Location:", 9)) {
                q += 9; while (*q == ' ') q++;
                int i = 0; while (*q && *q != '\r' && *q != '\n' && i < (int)sizeof loc - 1) loc[i++] = *q++;
                loc[i] = 0; break;
            }
        }
        free(buf);
        if (loc[0]) { depth++; int r = git_http(method, loc, ctype, req, reqlen, authb64, body, len, status); depth--; return r; }
        return -1;
    }

    size_t blen = total - (size_t)(hdrend - buf);
    uint8_t *out = malloc(blen ? blen : 1);
    if (!out) { free(buf); return -1; }
    memcpy(out, hdrend, blen);
    free(buf);
    *body = out; *len = blen;
    return 0;
}
