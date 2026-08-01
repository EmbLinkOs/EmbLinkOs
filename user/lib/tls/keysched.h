#ifndef EMBK_TLS_KEYSCHED_H
#define EMBK_TLS_KEYSCHED_H
/* TLS 1.3 key schedule (RFC 8446 §7.1) for the SHA-256 suites. The entire
 * schedule is HKDF-Extract plus HKDF-Expand-Label / Derive-Secret over our
 * HKDF-SHA256 (T1). Hash length is fixed at 32 (SHA-256); a SHA-384 suite would
 * parametrize TLS_HASH_LEN and the underlying HKDF. */
#include <stdint.h>
#include <stddef.h>

#define TLS_HASH_LEN 32   /* SHA-256 */

/* HKDF-Extract(salt, IKM) -> prk. salt/ikm are each one hash-length secret (or
 * NULL to mean a string of TLS_HASH_LEN zero bytes -- the RFC's "0"). */
void tls_extract(const uint8_t *salt, const uint8_t *ikm, uint8_t prk[TLS_HASH_LEN]);

/* HKDF-Expand-Label(secret, label, context, out_len) (RFC 8446 §7.1). `label`
 * is the bare label (e.g. "key"); the "tls13 " prefix is added here. */
void tls_expand_label(const uint8_t secret[TLS_HASH_LEN], const char *label,
                      const uint8_t *ctx, size_t ctx_len,
                      uint8_t *out, size_t out_len);

/* Derive-Secret(secret, label, transcript_hash) -> out[32]. transcript_hash is
 * the running Transcript-Hash of the handshake messages (or SHA-256("") when
 * the schedule step takes no messages, e.g. the "derived" steps). */
void tls_derive_secret(const uint8_t secret[TLS_HASH_LEN], const char *label,
                       const uint8_t transcript_hash[TLS_HASH_LEN],
                       uint8_t out[TLS_HASH_LEN]);

#endif /* EMBK_TLS_KEYSCHED_H */
