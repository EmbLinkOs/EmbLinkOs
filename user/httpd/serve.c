/* user/httpd/serve.c -- turning a URL path into bytes, safely. See serve.h.
 *
 * The only interesting question here is which files a request may reach, and
 * the answer is enforced TWICE, by two mechanisms that do not share a bug:
 *
 *   1. This resolver refuses ".." outright. It does not resolve it and then
 *      check where it landed -- it rejects the segment. A rule you can state
 *      in one sentence ("no dot-dot, ever") is one you can be sure of; a
 *      canonicalise-then-compare is a rule whose correctness depends on the
 *      canonicaliser agreeing with the filesystem about symlinks, case and
 *      trailing slashes, which is exactly where traversal bugs live.
 *
 *   2. The PROCESS cannot name anything outside its namespace. httpd is
 *      spawned with a namespace grant, so even a resolver bug reaches only
 *      what the server was given. Naming is owning (docs/USERSPACE_v2.md).
 *
 * Belt and braces, and neither is load-bearing alone.
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "embk.h"
#include "embk_socket.h"
#include "http.h"
#include "mime.h"
#include "serve.h"

/* Join root + urlpath, rejecting anything that could climb out. */
static int resolve(const char *root, const char *urlpath, char *out, size_t cap) {
    if (urlpath[0] != '/') return 0;

    size_t n = 0;
    /* the root, minus any trailing slash (so "/" and "/data/" both behave) */
    for (const char *p = root; *p && n + 1 < cap; p++) out[n++] = *p;
    while (n > 1 && out[n - 1] == '/') n--;

    const char *p = urlpath;
    while (*p) {
        while (*p == '/') p++;                 /* collapse // */
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - seg);

        if (sl == 1 && seg[0] == '.') continue;             /* "." is a no-op  */
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') return 0;  /* refuse    */
        if (n + sl + 2 >= cap) return 0;                    /* would truncate  */
        out[n++] = '/';
        memcpy(out + n, seg, sl);
        n += sl;
    }
    if (n == 0) out[n++] = '/';
    out[n] = 0;
    return 1;
}

/* HTML-escape into a fixed buffer. A filename is attacker-controlled input the
 * moment anyone else can write to the served directory. */
static void esc(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        switch (*s) {
            case '<': memcpy(out + o, "&lt;",   4); o += 4; break;
            case '>': memcpy(out + o, "&gt;",   4); o += 4; break;
            case '&': memcpy(out + o, "&amp;",  5); o += 5; break;
            case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
            default:  out[o++] = *s;                        break;
        }
    }
    out[o] = 0;
}

static void human(long n, char *buf, size_t cap) {
    if (n < 1024)              snprintf(buf, cap, "%ld B", n);
    else if (n < 1024L * 1024) snprintf(buf, cap, "%ld KB", (n + 512) / 1024);
    else                       snprintf(buf, cap, "%ld.%ld MB", n / (1024L*1024),
                                        ((n % (1024L*1024)) * 10) / (1024L*1024));
}

static int send_all(int fd, const char *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        long w = send(fd, b + off, n - off, 0);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* A directory listing. Built in one buffer and sent with a Content-Length,
 * because a length a browser can trust beats a connection-close it has to
 * infer -- and because it makes the whole response one write. */
static int serve_dir(int fd, const struct http_req *r, const char *fsdir) {
    static char page[64 * 1024];
    char epath[HTTP_TARGET_MAX * 2];
    esc(r->path, epath, sizeof epath);

    int n = snprintf(page, sizeof page,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>%s</title><style>"
        "body{font:14px system-ui,sans-serif;background:#16171c;color:#e0e2e8;margin:0;padding:28px}"
        "h1{font-size:16px;font-weight:600;margin:0 0 18px}"
        "a{color:#9aa4ff;text-decoration:none}a:hover{text-decoration:underline}"
        "table{border-collapse:collapse;width:100%%}"
        "td{padding:5px 10px;border-bottom:1px solid #24262e}"
        "td.s{text-align:right;color:#8b90a0;white-space:nowrap}"
        "footer{margin-top:22px;color:#666b7a;font-size:12px}"
        "</style></head><body><h1>%s</h1><table>",
        epath, epath);

    if (strcmp(r->path, "/") != 0)
        n += snprintf(page + n, sizeof page - n,
                      "<tr><td><a href=\"..\">../</a></td><td class=\"s\"></td></tr>");

    DIR *d = opendir(fsdir);
    if (!d) return http_error(fd, 403), 403;
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d)) != NULL && n < (int)sizeof page - 1024) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", fsdir, de->d_name);
        struct stat st;
        int isdir = 0; long sz = 0;
        if (stat(full, &st) == 0) { isdir = S_ISDIR(st.st_mode); sz = (long)st.st_size; }

        char ename[512]; esc(de->d_name, ename, sizeof ename);
        char size[24];
        if (isdir) snprintf(size, sizeof size, "--");
        else       human(sz, size, sizeof size);

        n += snprintf(page + n, sizeof page - n,
                      "<tr><td><a href=\"%s%s\">%s%s</a></td><td class=\"s\">%s</td></tr>",
                      ename, isdir ? "/" : "", ename, isdir ? "/" : "", size);
        count++;
    }
    closedir(d);
    n += snprintf(page + n, sizeof page - n,
                  "</table><footer>%d item%s &middot; served by EmbLinkOS</footer>"
                  "</body></html>\n", count, count == 1 ? "" : "s");

    http_write_head(fd, 200, "text/html; charset=utf-8", n);
    if (r->head_only) return 200;
    return send_all(fd, page, (size_t)n) == 0 ? 200 : -1;
}

static int serve_file(int fd, const struct http_req *r, const char *fspath, long size) {
    int in = (int)embk_open(fspath, EMBK_O_RDONLY, 0);
    if (in < 0) { http_error(fd, 403); return 403; }

    http_write_head(fd, 200, mime_for(fspath), size);
    if (r->head_only) { embk_close(in); return 200; }

    /* Streamed in 32 KB bites rather than read whole: the point of a server is
     * that it can serve something bigger than itself. */
    static char buf[32 * 1024];
    long n;
    while ((n = (long)embk_read(in, buf, sizeof buf)) > 0) {
        if (send_all(fd, buf, (size_t)n) != 0) { embk_close(in); return -1; }
    }
    embk_close(in);
    return 200;
}

int serve_request(int fd, const struct http_req *r, const char *root) {
    char fspath[1024];
    if (!resolve(root, r->path, fspath, sizeof fspath)) {
        http_error(fd, 403);
        return 403;
    }

    struct stat st;
    if (stat(fspath, &st) != 0) { http_error(fd, 404); return 404; }

    if (S_ISDIR(st.st_mode)) {
        /* index.html wins over a listing, the way every server does it, so a
         * directory can present itself rather than be enumerated. */
        char idx[1100];
        snprintf(idx, sizeof idx, "%s/index.html", fspath);
        struct stat ist;
        if (stat(idx, &ist) == 0 && !S_ISDIR(ist.st_mode))
            return serve_file(fd, r, idx, (long)ist.st_size);
        return serve_dir(fd, r, fspath);
    }
    return serve_file(fd, r, fspath, (long)st.st_size);
}
