#ifndef EMBK_TLS_BIGNUM_H
#define EMBK_TLS_BIGNUM_H
/* A fixed-width unsigned bignum with Montgomery modular arithmetic (docs/TLS.md
 * T3). Wide enough for both the NIST prime curves (ECDSA, up to P-384) and RSA
 * moduli up to 4096-bit. Limbs are 64-bit; the CIOS Montgomery multiply uses
 * __int128 for the 64x64 products (fine on x86-64, our only target). Every op
 * takes an explicit limb count, so a 256-bit ECDSA field costs the same as
 * before -- only the array size grew. For *verification* (public-key ops over
 * public values), so NOT constant-time. */
#include <stdint.h>
#include <stddef.h>

#define BN_MAX_LIMBS 64                /* 64 * 64 = 4096 bits (RSA-4096) */
typedef uint64_t bn[BN_MAX_LIMBS];

void bn_zero(bn a, int n);
void bn_copy(bn r, const bn a, int n);
int  bn_is_zero(const bn a, int n);
int  bn_cmp(const bn a, const bn b, int n);            /* -1 / 0 / 1 */
void bn_from_be(bn r, const uint8_t *in, size_t in_len, int n);  /* big-endian -> limbs */
void bn_to_be(uint8_t *out, const bn a, int n);        /* limbs -> n*8 big-endian bytes */

/* Montgomery context for one odd modulus m (< 2^(64*n)). */
struct mont {
    bn       m;
    int      n;                        /* limb count */
    uint64_t n0;                       /* -m^-1 mod 2^64 */
    bn       rr;                       /* R^2 mod m,  R = 2^(64n) */
    bn       one_mont;                 /* R   mod m   (== 1 in Montgomery form) */
};

/* Initialise from a big-endian modulus of `nbytes` bytes. */
void mont_init(struct mont *mo, const uint8_t *mod_be, size_t nbytes);

void mont_mul(const struct mont *mo, bn r, const bn a, const bn b);  /* r = a*b*R^-1 mod m */
void mont_to(const struct mont *mo, bn r, const bn a);               /* a  -> a*R  mod m */
void mont_from(const struct mont *mo, bn r, const bn a);            /* aR -> a    mod m */
void mont_add(const struct mont *mo, bn r, const bn a, const bn b);  /* (a+b) mod m */
void mont_sub(const struct mont *mo, bn r, const bn a, const bn b);  /* (a-b) mod m */
void mont_inv(const struct mont *mo, bn r, const bn a);             /* a^-1 (Montgomery in/out) */

/* r = base^exp mod m, base and r in the Montgomery domain, exp a big-endian
 * integer (the RSA public exponent). Square-and-multiply, MSB first. */
void mont_pow(const struct mont *mo, bn r, const bn base,
              const uint8_t *exp_be, size_t exp_len);

/* Reduce a big-endian integer of arbitrary length modulo m -> Montgomery-domain
 * result. Used to load a hash or a signature scalar into the mod-n field. */
void mont_load_be(const struct mont *mo, bn r, const uint8_t *in, size_t in_len);

#endif /* EMBK_TLS_BIGNUM_H */
