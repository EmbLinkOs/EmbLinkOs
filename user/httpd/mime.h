/* user/httpd/mime.h -- filename extension to content type.
 *
 * Its own file because it is the one part of a server that is pure policy: a
 * table someone will want to extend without reading anything else. */
#ifndef _EMBLINK_HTTPD_MIME_H_
#define _EMBLINK_HTTPD_MIME_H_

/* Never NULL. Unknown extensions get application/octet-stream, which makes a
 * browser download rather than guess -- guessing is how a text/html sniff on
 * an unknown file becomes a scripting bug. */
const char *mime_for(const char *path);

#endif
