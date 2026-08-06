/* user/httpd/serve.h -- the file-serving policy: which bytes a URL may reach.
 *
 * Separate from http.c on purpose. The protocol layer decides what a request
 * IS; this layer decides what it is ALLOWED to have. Mixing the two is how a
 * path check ends up depending on a header parse. */
#ifndef _EMBLINK_HTTPD_SERVE_H_
#define _EMBLINK_HTTPD_SERVE_H_

#include "http.h"

/* Serve `r` from beneath `root` on the connected socket `fd`. Writes the whole
 * response (including error responses). Returns the status code sent, or -1 if
 * the connection broke mid-body. */
int serve_request(int fd, const struct http_req *r, const char *root);

#endif
