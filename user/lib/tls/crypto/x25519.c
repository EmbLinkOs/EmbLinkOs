/* X25519 scalar multiplication (RFC 7748). Field arithmetic in GF(2^255-19) is
 * done in the compact 16-limb radix-2^16 representation (the field layout used
 * by the public-domain TweetNaCl): a field element is 16 signed 64-bit limbs,
 * which leaves ample headroom for the schoolbook multiply's carries before a
 * reduction pass. Chosen over a 5x51 layout because it is the easiest to audit
 * for correctness and constant-timedness -- every operation is a fixed-length
 * loop with no data-dependent branches. Reduction uses 2^256 = 38 (mod p). */
#include "x25519.h"
#include <stdint.h>

typedef int64_t gf[16];

/* 121665 = the Montgomery curve constant (a-2)/4 for Curve25519, as a field
 * element: low limb 0xDB41, next limb 1 (0x1DB41 == 121665). */
static const gf k121665 = {0xDB41, 1};

/* Carry-propagate one field element back into 16-bit limbs, folding the top
 * overflow via 2^256 = 38 (mod 2^255-19). */
static void carry(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/* Constant-time conditional swap of p and q when b==1. */
static void cswap(gf p, gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];  /* fold high half */
    for (int i = 0; i < 16; i++) o[i] = t[i];
    carry(o);
    carry(o);
}

static void fsq(gf o, const gf a) { fmul(o, a, a); }

/* Inverse via Fermat: a^(p-2) = a^(2^255-21). Fixed 255-step ladder, skipping
 * the two multiplies at bit positions 2 and 4 (the zero bits of p-2). */
static void finv(gf o, const gf a) {
    gf c;
    for (int i = 0; i < 16; i++) c[i] = a[i];
    for (int i = 253; i >= 0; i--) {
        fsq(c, c);
        if (i != 2 && i != 4) fmul(c, c, a);
    }
    for (int i = 0; i < 16; i++) o[i] = c[i];
}

static void unpack(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;   /* clear the top bit of the u-coordinate */
}

/* Reduce fully mod p and serialize little-endian. */
static void pack(uint8_t *o, const gf n) {
    gf t, m;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    carry(t); carry(t); carry(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        cswap(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = t[i] & 0xff;
        o[2 * i + 1] = t[i] >> 8;
    }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    for (int i = 0; i < 32; i++) e[i] = scalar[i];
    e[0]  &= 248;              /* clamp per RFC 7748 */
    e[31] &= 127;
    e[31] |= 64;

    gf x, a, b, c, d, ee, f;
    unpack(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; a[i] = c[i] = d[i] = 0; }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; i--) {
        int r = (e[i >> 3] >> (i & 7)) & 1;
        cswap(a, b, r);
        cswap(c, d, r);
        fadd(ee, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsq(d, ee);
        fsq(f, a);
        fmul(a, c, a);
        fmul(c, b, ee);
        fadd(ee, a, c);
        fsub(a, a, c);
        fsq(b, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsq(b, ee);
        cswap(a, b, r);
        cswap(c, d, r);
    }

    finv(c, c);
    fmul(a, a, c);
    pack(out, a);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t base[32] = { 9 };   /* u = 9, rest zero */
    x25519(out, scalar, base);
}
