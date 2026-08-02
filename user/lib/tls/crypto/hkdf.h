#ifndef EMBK_TLS_HKDF_H
#define EMBK_TLS_HKDF_H
/* HKDF-SHA256 (RFC 5869), the key-derivation function the TLS 1.3 key schedule
 * is built on (Extract-then-Expand). Thin layer over our HMAC-SHA256. */
#include <stdint.h>
#include <stddef.h>

#define HKDF_HASH_LEN 32   /* SHA-256 output */

/* PRK = HKDF-Extract(salt, IKM) = HMAC-SHA256(salt, IKM). A NULL/empty salt is
 * treated as HashLen zero bytes, per RFC 5869. */
void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm,  size_t ikm_len,
                  uint8_t prk[HKDF_HASH_LEN]);

/* OKM = HKDF-Expand(PRK, info, L). Returns 0, or -1 if L > 255*HashLen or the
 * (small, TLS-sized) info exceeds the internal bound. */
int hkdf_expand(const uint8_t prk[HKDF_HASH_LEN],
                const uint8_t *info, size_t info_len,
                uint8_t *okm, size_t okm_len);

#endif /* EMBK_TLS_HKDF_H */
