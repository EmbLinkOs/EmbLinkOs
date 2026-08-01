#ifndef EMBK_TLS_ECDSA_H
#define EMBK_TLS_ECDSA_H
/* ECDSA signature verification over the NIST prime curves P-256 and P-384
 * (docs/TLS.md T3) -- the signatures used throughout modern EC certificate
 * chains and the TLS 1.3 ecdsa_secp*_sha* CertificateVerify. Verify only (no
 * signing, no keygen); public-key math, so not constant-time. Curve arithmetic
 * is short-Weierstrass with a = -3 (true for both curves) in Jacobian
 * coordinates over the Montgomery bignum. */
#include <stdint.h>
#include <stddef.h>

struct ec_curve;
const struct ec_curve *ec_p256(void);
const struct ec_curve *ec_p384(void);

/* Field element size in bytes for a curve (32 for P-256, 48 for P-384). */
int ec_curve_bytes(const struct ec_curve *c);

/* Verify an ECDSA signature. `qx`,`qy` are the public-key affine coordinates,
 * each exactly ec_curve_bytes() big-endian bytes (from the SubjectPublicKeyInfo
 * point, 0x04 prefix stripped). `r`,`s` are the signature integers as big-endian
 * bytes of any length <= field size (DER leading zero already stripped). `hash`
 * is the message digest; if longer than the field it is truncated to the
 * leftmost field-size bytes (FIPS 186 leftmost-bits rule). Returns 1 if valid,
 * 0 otherwise. */
int ecdsa_verify(const struct ec_curve *c,
                 const uint8_t *qx, const uint8_t *qy,
                 const uint8_t *hash, size_t hashlen,
                 const uint8_t *r, size_t rlen,
                 const uint8_t *s, size_t slen);

#endif /* EMBK_TLS_ECDSA_H */
