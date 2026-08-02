#ifndef EMBK_TLS_RSA_H
#define EMBK_TLS_RSA_H
/* RSASSA-PKCS1-v1.5 signature verification (RFC 8017 §8.2.2), the signature
 * scheme most RSA certificate chains use (docs/TLS.md T3.5). Verify only: it is
 * s^e mod n (a public-exponent modexp over the Montgomery bignum) followed by an
 * EMSA-PKCS1-v1.5 padding + DigestInfo check. Moduli up to 4096-bit. */
#include <stdint.h>
#include <stddef.h>

enum { RSA_HASH_SHA256, RSA_HASH_SHA384, RSA_HASH_SHA512 };

/* Verify: modulus `n` and public exponent `e` (big-endian; a DER INTEGER's
 * leading zero is tolerated), `hash` is the message digest of `hash_alg`, `sig`
 * is the signature. Returns 1 if valid, 0 otherwise. */
int rsa_pkcs1_verify(const uint8_t *n, size_t n_len,
                     const uint8_t *e, size_t e_len,
                     int hash_alg, const uint8_t *hash, size_t hash_len,
                     const uint8_t *sig, size_t sig_len);

/* One-shot hash, e.g. sha256/sha384/sha512 -- used for MGF1 and H' in PSS. */
typedef void (*rsa_hash_fn)(const void *data, size_t len, uint8_t *out);

/* RSASSA-PSS verify (RFC 8017 §8.1.2 / §9.1.2) -- the scheme TLS 1.3 uses for
 * RSA CertificateVerify. `mhash` is Hash(M) (hlen bytes) already computed by the
 * caller; `hash`/`hlen` name that same hash for MGF1 + H'. The salt length is
 * recovered from the block. Returns 1 if valid, 0 otherwise. */
int rsa_pss_verify(const uint8_t *n, size_t n_len,
                   const uint8_t *e, size_t e_len,
                   rsa_hash_fn hash, size_t hlen,
                   const uint8_t *mhash,
                   const uint8_t *sig, size_t sig_len);

#endif /* EMBK_TLS_RSA_H */
