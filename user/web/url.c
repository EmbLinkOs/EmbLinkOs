/* user/web/url.c -- see url.h. Pure string work, no I/O. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "url.h"
#include "html.h"

int url_parse(const char *u, struct url *out) {
    if (!u || !out) return -1;
    memset(out, 0, sizeof *out);

    const char *p = u;
    if      (!strncmp(p, "https://", 8)) { out->kind = URL_HTTPS; out->port = 443; p += 8; }
    else if (!strncmp(p, "http://",  7)) { out->kind = URL_HTTP;  out->port = 80;  p += 7; }
    else if (!strncmp(p, "file://",  7)) { out->kind = URL_LOCAL; p += 7; }
    else if (*p == '/')                  { out->kind = URL_LOCAL; }
    else return -1;

    if (out->kind == URL_LOCAL) {
        snprintf(out->path, sizeof out->path, "%s", p);
        char *q = strchr(out->path, '?');
        if (q) { snprintf(out->query, sizeof out->query, "%s", q + 1); *q = 0; }
        return out->path[0] ? 0 : -1;
    }

    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < sizeof out->host) out->host[i++] = *p++;
    out->host[i] = 0;
    if (!i) return -1;

    if (*p == ':') {
        p++;
        out->port = atoi(p);
        while (*p && *p != '/') p++;
        if (out->port <= 0 || out->port > 65535) return -1;
    }
    if (*p == '/') snprintf(out->path, sizeof out->path, "%s", p);
    else           snprintf(out->path, sizeof out->path, "/");
    return 0;
}

int url_resolve(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !href[0] || !out || !cap) return -1;

    /* an absolute location ignores the base entirely */
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
        snprintf(out, cap, "%s", href);
        return 0;
    }

    /* A network base has a "://" to anchor on; hand those to the resolver that
     * already knows the rules (and is host-tested with them). */
    if (base && strstr(base, "://")) return html_resolve_url(base, href, out, cap);

    /* A LOCAL base has no authority component, so the same code cannot be
     * reused: there is no "root" to make a leading '/' relative to -- it IS
     * the root. Absolute paths pass through, and a bare name resolves against
     * the base's directory, which is what a link between two documents in
     * /system/web means. */
    if (href[0] == '/') { snprintf(out, cap, "%s", href); return 0; }
    if (!base || !base[0]) return -1;

    size_t dirlen = strlen(base);
    while (dirlen > 0 && base[dirlen - 1] != '/') dirlen--;
    if (!dirlen) return -1;
    snprintf(out, cap, "%.*s%s", (int)dirlen, base, href);
    return 0;
}
