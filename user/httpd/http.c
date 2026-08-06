/* user/httpd/http.c -- request parsing and response writing. See http.h. */
#include <stdio.h>
#include <string.h>
#include "embk_socket.h"
#include "http.h"

const char *http_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 414: return "URI Too Long";
        case 500: return "Internal Server Error";
        case 505: return "HTTP Version Not Supported";
        default:  return "Error";
    }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode in place-ish (src -> dst). A malformed escape is left
 * LITERAL rather than guessed at: inventing a byte for "%zz" is how a decoder
 * ends up disagreeing with the thing that validated the path. */
static void percent_decode(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '%' ) {
            int hi = hexval((unsigned char)src[i + 1]);
            int lo = hi >= 0 ? hexval((unsigned char)src[i + 2]) : -1;
            if (lo >= 0) { dst[o++] = (char)((hi << 4) | lo); i += 2; continue; }
        }
        dst[o++] = src[i];
    }
    dst[o] = 0;
}

int http_parse(const char *buf, size_t len, struct http_req *out) {
    memset(out, 0, sizeof *out);

    /* --- method --- */
    size_t i = 0;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') i++;
    if (i == 0 || i >= len || buf[i] != ' ') return 400;
    if (i >= sizeof out->method) return 400;
    memcpy(out->method, buf, i);
    out->method[i] = 0;
    i++;

    /* --- target --- */
    size_t ts = i;
    while (i < len && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') i++;
    size_t tlen = i - ts;
    if (tlen == 0) return 400;
    if (tlen >= sizeof out->target) return 414;
    memcpy(out->target, buf + ts, tlen);
    out->target[tlen] = 0;

    /* --- version (optional: HTTP/0.9 has none) --- */
    out->minor = 0;
    if (i < len && buf[i] == ' ') {
        i++;
        if (len - i >= 8 && memcmp(buf + i, "HTTP/1.", 7) == 0) {
            char m = buf[i + 7];
            if (m != '0' && m != '1') return 505;
            out->minor = m - '0';
        } else {
            return 505;
        }
    }

    /* --- path: query stripped, then percent-decoded --- */
    char raw[HTTP_TARGET_MAX];
    size_t n = 0;
    for (const char *p = out->target; *p && *p != '?' && *p != '#' && n + 1 < sizeof raw; p++)
        raw[n++] = *p;
    raw[n] = 0;
    percent_decode(raw, out->path, sizeof out->path);
    if (out->path[0] != '/') return 400;      /* only origin-form is served */

    if (!strcmp(out->method, "HEAD")) out->head_only = 1;
    else if (strcmp(out->method, "GET") != 0) return 405;
    return 0;
}

int http_write_head(int fd, int code, const char *ctype, long len) {
    char h[320];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: EmbLinkOS\r\n"
                     "Connection: close\r\n",
                     code, http_reason(code));
    if (ctype && n < (int)sizeof h)
        n += snprintf(h + n, sizeof h - n, "Content-Type: %s\r\n", ctype);
    if (len >= 0 && n < (int)sizeof h)
        n += snprintf(h + n, sizeof h - n, "Content-Length: %ld\r\n", len);
    if (n < (int)sizeof h) n += snprintf(h + n, sizeof h - n, "\r\n");
    return (int)send(fd, h, (size_t)n, 0);
}

int http_error(int fd, int code) {
    char body[192];
    int bl = snprintf(body, sizeof body,
                      "<!doctype html><html><body>"
                      "<h1>%d %s</h1><hr><p>EmbLinkOS</p></body></html>\n",
                      code, http_reason(code));
    http_write_head(fd, code, "text/html; charset=utf-8", bl);
    return (int)send(fd, body, (size_t)bl, 0);
}
