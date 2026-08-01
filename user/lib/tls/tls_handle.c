/* Opaque-handle wrapper over libtls -- see tls_handle.h for WHY. Compiled with
 * the full TLS include set (kshim + kernel + crypto), so it sees the real
 * `struct tls_conn`; its callers (e.g. CPython's _embtls) see only the opaque
 * forward declaration. */
#include "tls.h"
#include "tls_handle.h"
#include <stdlib.h>

struct tls_conn *tls_handle_new(void) {
    return (struct tls_conn *)calloc(1, sizeof(struct tls_conn));
}

int tls_handle_connect(struct tls_conn *c, int fd, const char *server_name) {
    if (!c) return -1;
    return tls_connect(c, fd, server_name);
}

long tls_handle_write(struct tls_conn *c, const void *buf, size_t len) {
    if (!c) return -1;
    return tls_write(c, buf, len);
}

long tls_handle_read(struct tls_conn *c, void *buf, size_t cap) {
    if (!c) return -1;
    return tls_read(c, buf, cap);
}

void tls_handle_free(struct tls_conn *c) {
    if (!c) return;
    tls_close(c);      /* best-effort close_notify + close(fd) */
    free(c);
}
