/* user/httpd/http.h -- the HTTP/1.x protocol layer, and nothing else.
 *
 * This file knows about request lines, headers and status codes. It does NOT
 * know what a file is, where the document root lives, or what a directory
 * listing looks like -- that is serve.c's business. Keeping the split honest
 * is what lets the protocol be tested by feeding it bytes, and lets the file
 * policy be reasoned about without HTTP in the way.
 */
#ifndef _EMBLINK_HTTPD_HTTP_H_
#define _EMBLINK_HTTPD_HTTP_H_

#include <stddef.h>

#define HTTP_TARGET_MAX 512

struct http_req {
    char method[12];                  /* "GET", "HEAD", ...                  */
    char target[HTTP_TARGET_MAX];     /* raw request target, query included  */
    char path[HTTP_TARGET_MAX];       /* percent-decoded path, query stripped */
    int  minor;                       /* HTTP/1.<minor>; 0 or 1              */
    int  head_only;                   /* HEAD: send headers, no body         */
};

/* Parse a request from `buf`. Returns 0 on success, or the HTTP status code to
 * reply with when the request is unusable (400 malformed, 414 too long, 505
 * unsupported version). Only the request line is interpreted; headers are
 * skipped deliberately -- this server needs nothing from them, and parsing
 * fields you do not use is how header-smuggling bugs are born. */
int http_parse(const char *buf, size_t len, struct http_req *out);

/* The reason phrase for a status code ("OK", "Not Found", ...). */
const char *http_reason(int code);

/* Write a complete response head. `len` < 0 omits Content-Length (only valid
 * when the connection close IS the framing). Always announces close: this
 * server does not keep connections alive, and saying so is what lets a client
 * stop waiting. */
int http_write_head(int fd, int code, const char *ctype, long len);

/* A complete, minimal error response (head + a one-line HTML body). */
int http_error(int fd, int code);

#endif /* _EMBLINK_HTTPD_HTTP_H_ */
