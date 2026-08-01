/* Fixed-width bignum + Montgomery arithmetic (see bignum.h). CIOS Montgomery
 * multiplication (Koc et al.), Newton's method for -m^-1 mod 2^64, and R^2 by
 * repeated modular doubling. Verification-only: not constant-time. */
#include "bignum.h"
#include <string.h>

typedef unsigned __int128 u128;

void bn_zero(bn a, int n)              { for (int i = 0; i < n; i++) a[i] = 0; }
void bn_copy(bn r, const bn a, int n)  { for (int i = 0; i < n; i++) r[i] = a[i]; }
int  bn_is_zero(const bn a, int n)     { uint64_t x = 0; for (int i = 0; i < n; i++) x |= a[i]; return x == 0; }

int bn_cmp(const bn a, const bn b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

/* r = a + b, returns carry out. */
static uint64_t bn_add(bn r, const bn a, const bn b, int n) {
    u128 c = 0;
    for (int i = 0; i < n; i++) { u128 s = (u128)a[i] + b[i] + c; r[i] = (uint64_t)s; c = s >> 64; }
    return (uint64_t)c;
}
/* r = a - b, returns borrow out. */
static uint64_t bn_sub(bn r, const bn a, const bn b, int n) {
    u128 borrow = 0;
    for (int i = 0; i < n; i++) {
        u128 s = (u128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)s;
        borrow = (s >> 64) & 1;
    }
    return (uint64_t)borrow;
}

void bn_from_be(bn r, const uint8_t *in, size_t in_len, int n) {
    bn_zero(r, n);
    for (size_t i = 0; i < in_len; i++) {
        size_t bytepos = in_len - 1 - i;      /* little-endian byte index */
        r[bytepos / 8] |= (uint64_t)in[i] << (8 * (bytepos % 8));
    }
}
void bn_to_be(uint8_t *out, const bn a, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 8; j++)
            out[(n - 1 - i) * 8 + (7 - j)] = (uint8_t)(a[i] >> (8 * j));
}

/* Newton's iteration for -m0^-1 mod 2^64 (m0 must be odd). */
static uint64_t mont_n0(uint64_t m0) {
    uint64_t x = 1;
    for (int i = 0; i < 6; i++) x *= 2 - m0 * x;   /* doubles correct bits each step */
    return (uint64_t)(0 - x);
}

/* r = a*b*R^-1 mod m (CIOS). */
void mont_mul(const struct mont *mo, bn r, const bn a, const bn b) {
    int n = mo->n;
    uint64_t t[BN_MAX_LIMBS + 2];
    for (int i = 0; i < n + 2; i++) t[i] = 0;

    for (int i = 0; i < n; i++) {
        u128 c = 0;
        for (int j = 0; j < n; j++) {
            u128 s = (u128)a[i] * b[j] + t[j] + (uint64_t)c;
            t[j] = (uint64_t)s; c = s >> 64;
        }
        u128 s = (u128)t[n] + c; t[n] = (uint64_t)s; t[n + 1] = (uint64_t)(s >> 64);

        uint64_t mp = t[0] * mo->n0;                 /* mod 2^64 */
        c = ((u128)mp * mo->m[0] + t[0]) >> 64;      /* t[0] becomes 0; carry out */
        for (int j = 1; j < n; j++) {
            u128 s2 = (u128)mp * mo->m[j] + t[j] + (uint64_t)c;
            t[j - 1] = (uint64_t)s2; c = s2 >> 64;
        }
        u128 s3 = (u128)t[n] + c; t[n - 1] = (uint64_t)s3;
        t[n] = t[n + 1] + (uint64_t)(s3 >> 64);
    }

    if (t[n] || bn_cmp(t, mo->m, n) >= 0) bn_sub(t, t, mo->m, n);
    bn_copy(r, t, n);
}

void mont_to(const struct mont *mo, bn r, const bn a)   { mont_mul(mo, r, a, mo->rr); }
void mont_from(const struct mont *mo, bn r, const bn a) {
    bn one; bn_zero(one, mo->n); one[0] = 1;
    mont_mul(mo, r, a, one);
}

void mont_add(const struct mont *mo, bn r, const bn a, const bn b) {
    uint64_t carry = bn_add(r, a, b, mo->n);
    if (carry || bn_cmp(r, mo->m, mo->n) >= 0) bn_sub(r, r, mo->m, mo->n);
}
void mont_sub(const struct mont *mo, bn r, const bn a, const bn b) {
    uint64_t borrow = bn_sub(r, a, b, mo->n);
    if (borrow) bn_add(r, r, mo->m, mo->n);          /* add modulus back */
}

/* a^-1 mod m via Fermat: a^(m-2). In/out are Montgomery-domain. m is prime. */
void mont_inv(const struct mont *mo, bn r, const bn a) {
    bn e; bn_copy(e, mo->m, mo->n);
    /* e = m - 2 */
    bn two; bn_zero(two, mo->n); two[0] = 2;
    bn_sub(e, e, two, mo->n);

    bn result; bn_copy(result, mo->one_mont, mo->n);  /* 1 in Montgomery form */
    bn base;   bn_copy(base, a, mo->n);
    for (int i = 0; i < mo->n; i++) {
        uint64_t limb = e[i];
        for (int b = 0; b < 64; b++) {
            if (limb & 1) mont_mul(mo, result, result, base);
            mont_mul(mo, base, base, base);
            limb >>= 1;
        }
    }
    bn_copy(r, result, mo->n);
}

void mont_init(struct mont *mo, const uint8_t *mod_be, size_t nbytes) {
    int n = (int)((nbytes + 7) / 8);
    mo->n = n;
    bn_from_be(mo->m, mod_be, nbytes, n);
    mo->n0 = mont_n0(mo->m[0]);

    /* R^2 mod m = 2^(128n) mod m, by doubling 1 that many times. */
    bn t; bn_zero(t, n); t[0] = 1;
    for (int i = 0; i < 128 * n; i++) {
        uint64_t carry = bn_add(t, t, t, n);         /* t <<= 1 */
        if (carry || bn_cmp(t, mo->m, n) >= 0) bn_sub(t, t, mo->m, n);
    }
    bn_copy(mo->rr, t, n);

    /* 1 in Montgomery form = R mod m = 2^(64n) mod m. */
    bn one; bn_zero(one, n); one[0] = 1;
    mont_mul(mo, mo->one_mont, one, mo->rr);         /* 1 * R^2 * R^-1 = R mod m */
}

void mont_load_be(const struct mont *mo, bn r, const uint8_t *in, size_t in_len) {
    /* Reduce a big-endian integer mod m by Horner: acc = acc*256 + byte, keeping
     * one extra top limb for the overflow the *256 produces, then subtracting m
     * (<=255 times per step, since acc < m before each multiply). in_len is
     * small (a hash or a signature scalar), so this is cheap. */
    int n = mo->n;
    uint64_t acc[BN_MAX_LIMBS + 1];
    for (int i = 0; i <= n; i++) acc[i] = 0;

    for (size_t i = 0; i < in_len; i++) {
        u128 carry = in[i];
        for (int j = 0; j <= n; j++) {               /* acc = acc*256 + byte */
            u128 s = ((u128)acc[j] << 8) + carry;
            acc[j] = (uint64_t)s; carry = s >> 64;
        }
        while (acc[n] != 0 || bn_cmp(acc, mo->m, n) >= 0) {
            uint64_t borrow = bn_sub(acc, acc, mo->m, n);
            acc[n] -= borrow;
        }
    }
    mont_to(mo, r, acc);                              /* acc < m, now n limbs */
}
