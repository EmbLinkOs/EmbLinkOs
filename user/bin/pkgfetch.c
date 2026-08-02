/* pkgfetch -- a native package installer for EmbLinkOS: "pip install" in spirit,
 * for pure-Python wheels, without needing TLS inside Python. It fetches PyPI's
 * PEP-503 simple index for a package over our OWN authenticated TLS (libtls),
 * picks a pure-Python wheel (`*-none-any.whl`), downloads it (also over TLS), and
 * unzips it into a site-packages directory. After that, `PYTHONPATH=<dir> python
 * -c "import <pkg>"` works -- a real PyPI package, pulled and installed by the OS.
 *
 * Scope (honest): pure-Python wheels only -- packages with C extensions need a
 * compile step this tool doesn't do. No dependency resolution (one package at a
 * time). HTTP/1.0 + Connection: close (no chunked). See docs/TODO.md.
 *
 * usage: pkgfetch [-d destdir] <package>
 * exit:  0 installed;  1 usage  2 index fetch  3 no pure wheel  4 wheel fetch
 *        5 unzip/install
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "embk_socket.h"
#include "tls.h"
#include "unzip.h"

#define MAX_BODY (8 * 1024 * 1024)   /* wheels + indexes fit comfortably */

/* ---- HTTPS GET over libtls (authenticated), following one redirect --------- */

static int split_url(const char *url, char *host, int hostsz, char *path, int pathsz) {
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

/* GET url -> malloc'd body (caller frees), *len set, *status set. Returns 0 ok. */
static int https_get(const char *url, uint8_t **body, size_t *len, int *status, int depth) {
    if (depth > 3) return -1;
    char host[256], path[1024];
    if (split_url(url, host, sizeof host, path, sizeof path) != 0) return -1;

    struct in_addr addr;
    if (emb_resolve(host, &addr) != 0) { fprintf(stderr, "pkgfetch: resolve %s failed\n", host); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(443); sa.sin_addr = addr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }

    struct tls_conn *c = malloc(sizeof *c);
    if (!c) { close(fd); return -1; }
    int trc = tls_connect(c, fd, host);
    if (trc != 0) {
        fprintf(stderr, "pkgfetch: TLS to %s failed (rc=%d)\n", host, trc);
        free(c); close(fd); return -1;
    }

    char req[1600];
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: EmbLinkOS-pkgfetch\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    tls_write(c, req, (size_t)rl);

    uint8_t *buf = malloc(MAX_BODY);
    if (!buf) { tls_close(c); free(c); return -1; }
    size_t total = 0;
    for (;;) {
        long n = tls_read(c, buf + total, MAX_BODY - total);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= MAX_BODY) break;
    }
    tls_close(c); free(c);

    /* Split header/body at CRLFCRLF; read status; follow one redirect. */
    uint8_t *hdrend = NULL;
    for (size_t i = 0; i + 3 < total; i++)
        if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') { hdrend = buf + i + 4; break; }
    if (!hdrend) { free(buf); return -1; }
    int st = 0;
    if (!strncmp((char *)buf, "HTTP/1.", 7)) st = atoi((char *)buf + 9);
    *status = st;

    if (st >= 300 && st < 400) {                 /* redirect: find Location: */
        char loc[1024] = {0};
        for (char *q = (char *)buf; q < (char *)hdrend; q++) {
            if ((q == (char *)buf || q[-1] == '\n') &&
                (!strncasecmp(q, "Location:", 9))) {
                q += 9; while (*q == ' ') q++;
                int i = 0; while (*q && *q != '\r' && *q != '\n' && i < (int)sizeof loc - 1) loc[i++] = *q++;
                loc[i] = 0; break;
            }
        }
        free(buf);
        if (loc[0]) return https_get(loc, body, len, status, depth + 1);
        return -1;
    }

    size_t blen = total - (size_t)(hdrend - buf);
    uint8_t *b = malloc(blen ? blen : 1);
    memcpy(b, hdrend, blen);
    free(buf);
    *body = b; *len = blen;
    return 0;
}

/* ---- wheel install: write each entry under destdir, mkdir -p parents ------- */

static const char *g_dest;

static void mkdirs(const char *path) {
    char tmp[1024]; strncpy(tmp, path, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

static int install_cb(void *ctx, const char *name, size_t nl, const uint8_t *data, size_t dl) {
    (void)ctx;
    char full[1024];
    int n = snprintf(full, sizeof full, "%s/%.*s", g_dest, (int)nl, name);
    if (n <= 0 || n >= (int)sizeof full) return -1;
    mkdirs(full);
    int fd = open(full, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "pkgfetch: cannot write %s\n", full); return -1; }
    size_t off = 0;
    while (off < dl) {
        long w = write(fd, data + off, dl - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

/* ---- simple-index scrape: pick a pure-Python wheel URL --------------------- */

/* Find the LAST href to a "*-none-any.whl" in the PEP-503 HTML (usually newest).
 * Copies the URL (minus any #fragment) into out. Returns 0 if found. */
static int pick_wheel(const uint8_t *html, size_t len, char *out, int outsz) {
    int found = 0;
    for (size_t i = 0; i + 6 < len; i++) {
        if (strncasecmp((const char *)html + i, "href=\"", 6) != 0) continue;
        const char *u = (const char *)html + i + 6;
        const char *end = u;
        while (end < (const char *)html + len && *end != '"' && *end != '#') end++;
        size_t ulen = (size_t)(end - u);
        /* is the file (before ?query) a pure-python wheel? */
        if (ulen > 14 && ulen < (size_t)outsz) {
            /* check it ends in "-none-any.whl" */
            const char *suf = "-none-any.whl";
            size_t sl = strlen(suf);
            /* the filename may be followed by nothing (# stripped) */
            if (ulen >= sl && memcmp(u + ulen - sl, suf, sl) == 0) {
                memcpy(out, u, ulen); out[ulen] = 0; found = 1;   /* keep last */
            }
        }
    }
    return found ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *dest = "/data/py/site-packages";
    const char *pkg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dest = argv[++i];
        else pkg = argv[i];
    }
    if (!pkg) { fprintf(stderr, "usage: pkgfetch [-d destdir] <package>\n"); return 1; }
    g_dest = dest;

    char idx_url[512];
    snprintf(idx_url, sizeof idx_url, "https://pypi.org/simple/%s/", pkg);
    fprintf(stderr, "pkgfetch: index %s\n", idx_url);

    uint8_t *idx; size_t ilen; int st;
    if (https_get(idx_url, &idx, &ilen, &st, 0) != 0 || st / 100 != 2) {
        fprintf(stderr, "pkgfetch: index fetch failed (HTTP %d)\n", st); return 2;
    }
    char wurl[1024];
    if (pick_wheel(idx, ilen, wurl, sizeof wurl) != 0) {
        fprintf(stderr, "pkgfetch: no pure-Python wheel for '%s' (needs a compiled build?)\n", pkg);
        free(idx); return 3;
    }
    free(idx);
    fprintf(stderr, "pkgfetch: wheel %s\n", wurl);

    uint8_t *whl; size_t wlen;
    if (https_get(wurl, &whl, &wlen, &st, 0) != 0 || st / 100 != 2) {
        fprintf(stderr, "pkgfetch: wheel fetch failed (HTTP %d)\n", st); return 4;
    }
    fprintf(stderr, "pkgfetch: downloaded %zu bytes, installing into %s\n", wlen, dest);

    mkdir("/data", 0755); mkdir("/data/py", 0755); mkdir(dest, 0755);
    int r = unzip_iter(whl, wlen, install_cb, NULL);
    free(whl);
    if (r != 0) { fprintf(stderr, "pkgfetch: install failed (%d)\n", r); return 5; }

    fprintf(stderr, "pkgfetch: installed '%s' -> %s   (run: PYTHONPATH=%s python -c 'import %s')\n",
            pkg, dest, dest, pkg);
    return 0;
}
