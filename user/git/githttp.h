#ifndef EMBK_GITHTTP_H
#define EMBK_GITHTTP_H
/* Git smart-HTTP transport over libtls (docs/TLS.md). The one place that speaks
 * HTTPS for the clone tool: git can't use its own transport here (it fork/execs
 * git-remote-https and index-pack, and EmbLink has no fork/exec), so we drive
 * the smart-HTTP protocol ourselves. IPv4 + TLS 1.3 only, one request per
 * connection (HTTP/1.0 Connection: close), body read to EOF. */
#include <stddef.h>
#include <stdint.h>

/* One HTTPS request. `method` is "GET" or "POST". For POST, `ctype` is the
 * Content-Type and req/reqlen the body (NULL/0 for GET). On success returns 0,
 * sets *status to the HTTP status, and *body and *len to a malloc'd response body
 * (caller frees). Follows up to 3 redirects. Negative on transport/TLS error. */
int git_http(const char *method, const char *url, const char *ctype,
             const uint8_t *req, size_t reqlen,
             uint8_t **body, size_t *len, int *status);

#endif /* EMBK_GITHTTP_H */
