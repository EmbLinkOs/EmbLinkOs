#ifndef EMBK_TLS_X25519_H
#define EMBK_TLS_X25519_H
/* X25519 (RFC 7748) -- the ECDHE key exchange TLS 1.3 uses universally. A
 * scalar multiplication on Curve25519 via the Montgomery ladder, over the prime
 * field GF(2^255 - 19). Brand-new crypto (no kernel equivalent), written for
 * the TLS campaign (docs/TLS.md §1.5). Constant-time by construction: the ladder
 * runs a fixed 255 steps and branches only through arithmetic selects. */
#include <stdint.h>

#define X25519_KEY_LEN 32

/* out(u-coordinate) = scalar * point. `point` is a peer u-coordinate; pass the
 * base point (u=9) to turn a private scalar into a public key. The scalar is
 * clamped per RFC 7748 internally, so a raw 32-byte random value is a valid
 * private key. */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

/* Public key = X25519(scalar, 9). */
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#endif /* EMBK_TLS_X25519_H */
