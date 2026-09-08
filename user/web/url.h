/* user/web/url.h -- what a location IS, before anything tries to fetch it.
 *
 * Kept apart from net.c on purpose. Deciding what "/system/web/index.html",
 * "http://10.0.2.2:8080/x", and "about.html" each MEAN is pure string work with
 * no I/O in it, which makes it the part worth testing on the host and the part
 * a fetch should not be reasoning about while it holds a socket.
 */
#ifndef _EMBLINK_WEB_URL_H_
#define _EMBLINK_WEB_URL_H_

#include <stddef.h>

enum url_kind {
    URL_LOCAL = 0,   /* a filesystem path -- what B1 could already read */
    URL_HTTP,
    URL_HTTPS,
};

struct url {
    int  kind;
    char host[128];
    int  port;
    char path[512];   /* the request target; for URL_LOCAL, the file path */
    /* A LOCAL url's query is split OFF the path: "/a.html?q=x" must open
     * a.html, while the address bar still shows the whole thing and a script
     * can still read the query. A network path keeps its query, because the
     * server is the one that parses it. */
    char query[256];
};

/* Split an absolute location. Anything that is not http:// or https:// and
 * begins with '/' is a local path -- which keeps every B1 document working
 * unchanged, and means the address bar takes both without a mode switch.
 * Returns 0, or -1 if it is neither. */
int url_parse(const char *url, struct url *out);

/* Resolve `href` against `base`, for a link in a document. Handles a LOCAL
 * base (which the network resolver cannot, having no "://" to anchor on) and
 * delegates network bases to html_resolve_url. Returns 0 or -1. */
int url_resolve(const char *base, const char *href, char *out, size_t cap);

#endif /* _EMBLINK_WEB_URL_H_ */
