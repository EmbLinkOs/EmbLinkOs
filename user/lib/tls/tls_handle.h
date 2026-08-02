#ifndef EMBK_TLS_HANDLE_H
#define EMBK_TLS_HANDLE_H
/* An OPAQUE-handle wrapper over libtls (tls.h). It exists to keep libtls's
 * ~50 KB `struct tls_conn` -- and the kernel/kshim include world its full
 * definition drags in (the crypto headers, include/types.h) -- OUT of scope for consumers
 * that must ALSO include a foreign, type-sensitive header. The first such
 * consumer is CPython's `_embtls` module, which cannot include tls.h and
 * Python.h in the same TU without the two type worlds colliding.
 *
 * tls_handle.c is compiled with the TLS include set (kshim + kernel + crypto);
 * this header is deliberately newlib-clean (only <stdint.h>/<stddef.h> and a
 * forward declaration), so it composes with anything. Same compile-boundary
 * discipline as the rest of the TLS campaign (docs/TLS.md). */
#include <stdint.h>
#include <stddef.h>

struct tls_conn;   /* opaque here; fully defined only inside tls_handle.c */

/* Allocate a zeroed connection on the heap (it is far too large for a stack).
 * Returns NULL on OOM. Pair with tls_handle_free(). */
struct tls_conn *tls_handle_new(void);

/* Run the TLS 1.3 handshake over an already-TCP-connected `fd`, sending
 * `server_name` as SNI. Returns 0 on success, negative on error (the tls.h
 * codes: -1 I/O/protocol, -2 unsupported HelloRetryRequest, -3 bad Finished,
 * and the certificate-verification failures). Does NOT take ownership of fd. */
int tls_handle_connect(struct tls_conn *c, int fd, const char *server_name);

/* Application data. write returns bytes accepted or -1; read returns >0 bytes,
 * 0 on clean close (close_notify/EOF), or -1 on error. */
long tls_handle_write(struct tls_conn *c, const void *buf, size_t len);
long tls_handle_read(struct tls_conn *c, void *buf, size_t cap);

/* Best-effort close_notify + close(fd), then free the handle. NULL-safe. */
void tls_handle_free(struct tls_conn *c);

#endif /* EMBK_TLS_HANDLE_H */
