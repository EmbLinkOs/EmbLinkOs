/* user/httpd/mime.c -- see mime.h. */
#include <string.h>
#include "mime.h"

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

const char *mime_for(const char *path) {
    /* A text type carries charset=utf-8 because the OS writes UTF-8 and a browser
     * that has to guess will pick Latin-1 and mangle every non-ASCII byte. */
    if (ends_with(path, ".html") || ends_with(path, ".htm")) return "text/html; charset=utf-8";
    if (ends_with(path, ".txt")  || ends_with(path, ".md"))  return "text/plain; charset=utf-8";
    if (ends_with(path, ".c")    || ends_with(path, ".h"))   return "text/plain; charset=utf-8";
    if (ends_with(path, ".conf") || ends_with(path, ".ns")
        || ends_with(path, ".app") || ends_with(path, ".log")) return "text/plain; charset=utf-8";
    if (ends_with(path, ".css"))  return "text/css; charset=utf-8";
    if (ends_with(path, ".js"))   return "text/javascript; charset=utf-8";
    if (ends_with(path, ".json")) return "application/json";
    if (ends_with(path, ".png"))  return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".gif"))  return "image/gif";
    if (ends_with(path, ".svg"))  return "image/svg+xml";
    if (ends_with(path, ".ttf"))  return "font/ttf";
    return "application/octet-stream";
}
