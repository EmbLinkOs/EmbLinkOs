#ifndef EMBK_TLS_BIGNUM_H
#define EMBK_TLS_BIGNUM_H
/* A fixed-width unsigned bignum with Montgomery modular arithmetic, sized for
 * the NIST prime curves up to P-384 (docs/TLS.md T3). Limbs are 64-bit; the CIOS
 * Montgomery multiply uses __int128 for the 64x64 products (fine on x86-64, our
 * only target). This is for ECDSA *verification* -- a public-key operation over
 * public values -- so it is NOT written to be constant-time. */
#include <stdint.h>
#include <stddef.h>

#define BN_MAX_LIMBS 6                 /* 6 * 64 = 384 bits (P-384) */
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

/* Reduce a big-endian integer of arbitrary length modulo m -> Montgomery-domain
 * result. Used to load a hash or a signature scalar into the mod-n field. */
void mont_load_be(const struct mont *mo, bn r, const uint8_t *in, size_t in_len);

#endif /* EMBK_TLS_BIGNUM_H */
