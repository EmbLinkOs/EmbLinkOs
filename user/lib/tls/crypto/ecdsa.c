/* ECDSA verify over P-256 / P-384 (see ecdsa.h). Jacobian point arithmetic with
 * a = -3 (EFD dbl-2001-b / add-2007-bl formulas), all field elements in the
 * Montgomery domain. */
#include "ecdsa.h"
#include "bignum.h"
#include <string.h>

struct ec_curve {
    int         nbytes;
    struct mont fp;          /* field, mod p */
    struct mont fn;          /* scalars, mod n */
    bn          gx, gy;      /* base point G, Montgomery form (mod p) */
    int         ready;
};

/* --- curve parameters (big-endian) --------------------------------------- */

static const uint8_t P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const uint8_t P256_N[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const uint8_t P256_GX[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const uint8_t P256_GY[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };

static const uint8_t P384_P[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff };
static const uint8_t P384_N[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,
    0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73 };
static const uint8_t P384_GX[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,
    0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,
    0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7 };
static const uint8_t P384_GY[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,
    0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,
    0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f };

static struct ec_curve g_p256, g_p384;

static void curve_init(struct ec_curve *c, int nbytes,
                       const uint8_t *p, const uint8_t *n,
                       const uint8_t *gx, const uint8_t *gy) {
    if (c->ready) return;
    c->nbytes = nbytes;
    mont_init(&c->fp, p, nbytes);
    mont_init(&c->fn, n, nbytes);
    bn t;
    bn_from_be(t, gx, nbytes, c->fp.n); mont_to(&c->fp, c->gx, t);
    bn_from_be(t, gy, nbytes, c->fp.n); mont_to(&c->fp, c->gy, t);
    c->ready = 1;
}

const struct ec_curve *ec_p256(void) { curve_init(&g_p256, 32, P256_P, P256_N, P256_GX, P256_GY); return &g_p256; }
const struct ec_curve *ec_p384(void) { curve_init(&g_p384, 48, P384_P, P384_N, P384_GX, P384_GY); return &g_p384; }
int ec_curve_bytes(const struct ec_curve *c) { return c->nbytes; }

/* --- small field-constant multiplies (Montgomery domain) ----------------- */
static void f2(const struct mont *fp, bn r, const bn a) { mont_add(fp, r, a, a); }
static void f3(const struct mont *fp, bn r, const bn a) { bn t; f2(fp, t, a); mont_add(fp, r, t, a); }
static void f4(const struct mont *fp, bn r, const bn a) { f2(fp, r, a); f2(fp, r, r); }
static void f8(const struct mont *fp, bn r, const bn a) { f4(fp, r, a); f2(fp, r, r); }

/* --- Jacobian points; Z == 0 is the point at infinity -------------------- */
struct jpoint { bn X, Y, Z; };

static void jdouble(const struct ec_curve *c, struct jpoint *o, const struct jpoint *P) {
    const struct mont *fp = &c->fp;
    if (bn_is_zero(P->Z, fp->n)) { *o = *P; return; }
    bn delta, gamma, beta, alpha, t1, t2, X3, Y3, Z3, tmp;
    mont_mul(fp, delta, P->Z, P->Z);           /* Z^2 */
    mont_mul(fp, gamma, P->Y, P->Y);           /* Y^2 */
    mont_mul(fp, beta, P->X, gamma);           /* X*gamma */
    mont_sub(fp, t1, P->X, delta);
    mont_add(fp, t2, P->X, delta);
    mont_mul(fp, t1, t1, t2);                  /* (X-Z^2)(X+Z^2) */
    f3(fp, alpha, t1);                         /* 3*(...) = 3X^2 - 3Z^4 */
    mont_mul(fp, X3, alpha, alpha);
    f8(fp, tmp, beta);
    mont_sub(fp, X3, X3, tmp);                 /* X3 = alpha^2 - 8*beta */
    mont_add(fp, t1, P->Y, P->Z); mont_mul(fp, Z3, t1, t1);
    mont_sub(fp, Z3, Z3, gamma); mont_sub(fp, Z3, Z3, delta);   /* Z3 = (Y+Z)^2 - gamma - delta */
    f4(fp, t2, beta); mont_sub(fp, t1, t2, X3);
    mont_mul(fp, Y3, alpha, t1);
    mont_mul(fp, tmp, gamma, gamma); f8(fp, tmp, tmp);
    mont_sub(fp, Y3, Y3, tmp);                 /* Y3 = alpha*(4beta - X3) - 8*gamma^2 */
    bn_copy(o->X, X3, fp->n); bn_copy(o->Y, Y3, fp->n); bn_copy(o->Z, Z3, fp->n);
}

static void jadd(const struct ec_curve *c, struct jpoint *o, const struct jpoint *P, const struct jpoint *Q) {
    const struct mont *fp = &c->fp;
    if (bn_is_zero(P->Z, fp->n)) { *o = *Q; return; }
    if (bn_is_zero(Q->Z, fp->n)) { *o = *P; return; }
    bn Z1Z1, Z2Z2, U1, U2, S1, S2, t;
    mont_mul(fp, Z1Z1, P->Z, P->Z);
    mont_mul(fp, Z2Z2, Q->Z, Q->Z);
    mont_mul(fp, U1, P->X, Z2Z2);
    mont_mul(fp, U2, Q->X, Z1Z1);
    mont_mul(fp, t, Q->Z, Z2Z2); mont_mul(fp, S1, P->Y, t);   /* Y1*Z2^3 */
    mont_mul(fp, t, P->Z, Z1Z1); mont_mul(fp, S2, Q->Y, t);   /* Y2*Z1^3 */
    if (bn_cmp(U1, U2, fp->n) == 0) {
        if (bn_cmp(S1, S2, fp->n) != 0) { bn_zero(o->Z, fp->n); return; }  /* P = -Q */
        jdouble(c, o, P); return;
    }
    bn H, I, J, r, V, X3, Y3, Z3, t2, twoH;
    mont_sub(fp, H, U2, U1);
    f2(fp, twoH, H); mont_mul(fp, I, twoH, twoH);   /* (2H)^2 */
    mont_mul(fp, J, H, I);
    mont_sub(fp, t, S2, S1); f2(fp, r, t);          /* 2(S2-S1) */
    mont_mul(fp, V, U1, I);
    mont_mul(fp, X3, r, r); mont_sub(fp, X3, X3, J);
    f2(fp, t2, V); mont_sub(fp, X3, X3, t2);        /* X3 = r^2 - J - 2V */
    mont_sub(fp, t, V, X3); mont_mul(fp, Y3, r, t);
    mont_mul(fp, t2, S1, J); f2(fp, t2, t2); mont_sub(fp, Y3, Y3, t2);  /* Y3 = r(V-X3) - 2*S1*J */
    mont_add(fp, t, P->Z, Q->Z); mont_mul(fp, Z3, t, t);
    mont_sub(fp, Z3, Z3, Z1Z1); mont_sub(fp, Z3, Z3, Z2Z2); mont_mul(fp, Z3, Z3, H);
    bn_copy(o->X, X3, fp->n); bn_copy(o->Y, Y3, fp->n); bn_copy(o->Z, Z3, fp->n);
}

/* R = k*P, double-and-add over the bits of k (MSB first). */
static void jscalar(const struct ec_curve *c, struct jpoint *o, const bn k, const struct jpoint *P) {
    const struct mont *fp = &c->fp;
    struct jpoint R;
    bn_copy(R.X, fp->one_mont, fp->n); bn_copy(R.Y, fp->one_mont, fp->n); bn_zero(R.Z, fp->n);
    for (int i = c->nbytes * 8 - 1; i >= 0; i--) {
        jdouble(c, &R, &R);
        if ((k[i / 64] >> (i % 64)) & 1) jadd(c, &R, &R, P);
    }
    *o = R;
}

int ecdsa_verify(const struct ec_curve *c,
                 const uint8_t *qx, const uint8_t *qy,
                 const uint8_t *hash, size_t hashlen,
                 const uint8_t *r, size_t rlen,
                 const uint8_t *s, size_t slen) {
    const struct mont *fn = &c->fn, *fp = &c->fp;
    int nb = c->nbytes;
    if (rlen > (size_t)nb || slen > (size_t)nb) return 0;

    /* Load r, s as plain integers; require 1 <= r,s < n. */
    bn rr, ss;
    bn_from_be(rr, r, rlen, fn->n);
    bn_from_be(ss, s, slen, fn->n);
    if (bn_is_zero(rr, fn->n) || bn_cmp(rr, fn->m, fn->n) >= 0) return 0;
    if (bn_is_zero(ss, fn->n) || bn_cmp(ss, fn->m, fn->n) >= 0) return 0;

    /* e = leftmost field-size bytes of the hash, reduced mod n. */
    size_t elen = hashlen > (size_t)nb ? (size_t)nb : hashlen;
    bn em; mont_load_be(fn, em, hash, elen);   /* Montgomery form of (e mod n) */

    /* w = s^-1 mod n; u1 = e*w; u2 = r*w  (Montgomery domain in fn). */
    bn sm, wm, rm, u1m, u2m, u1, u2;
    mont_to(fn, sm, ss);
    mont_inv(fn, wm, sm);
    mont_mul(fn, u1m, em, wm);
    mont_to(fn, rm, rr);
    mont_mul(fn, u2m, rm, wm);
    mont_from(fn, u1, u1m);                     /* plain scalars for the ladder */
    mont_from(fn, u2, u2m);

    /* Base point G and public key Q as Jacobian points (Z = 1). */
    struct jpoint G, Q;
    bn_copy(G.X, c->gx, fp->n); bn_copy(G.Y, c->gy, fp->n); bn_copy(G.Z, fp->one_mont, fp->n);
    bn tq;
    bn_from_be(tq, qx, nb, fp->n); mont_to(fp, Q.X, tq);
    bn_from_be(tq, qy, nb, fp->n); mont_to(fp, Q.Y, tq);
    bn_copy(Q.Z, fp->one_mont, fp->n);

    /* R = u1*G + u2*Q. */
    struct jpoint A, B, Rp;
    jscalar(c, &A, u1, &G);
    jscalar(c, &B, u2, &Q);
    jadd(c, &Rp, &A, &B);
    if (bn_is_zero(Rp.Z, fp->n)) return 0;      /* R at infinity: invalid */

    /* affine x = X/Z^2, then v = x mod n; valid iff v == r. */
    bn zinv, zinv2, xm, xplain;
    mont_inv(fp, zinv, Rp.Z);
    mont_mul(fp, zinv2, zinv, zinv);
    mont_mul(fp, xm, Rp.X, zinv2);
    mont_from(fp, xplain, xm);

    uint8_t xb[48];
    bn_to_be(xb, xplain, fp->n);
    bn vm, v;
    mont_load_be(fn, vm, xb, nb);
    mont_from(fn, v, vm);
    return bn_cmp(v, rr, fn->n) == 0;
}
