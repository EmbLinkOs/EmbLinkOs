/* AES-GCM AEAD (NIST SP 800-38D). GHASH is multiplication in GF(2^128) with the
 * reduction polynomial x^128 + x^7 + x^2 + x + 1 (the constant 0xe1 below is its
 * top byte); encryption is AES in counter mode starting from J0 = IV || 1. This
 * is the textbook bit-serial GHASH -- correct and small, not fast; a table-driven
 * version can replace it if AEAD throughput ever matters. */
#include "gcm.h"
#include <string.h>

static void put_be64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* Y <- Y . H  in GF(2^128), bytes in big-endian bit order (SP 800-38D). */
static void ghash_mul(uint8_t Y[16], const uint8_t H[16]) {
    uint8_t Z[16] = {0}, V[16];
    memcpy(V, H, 16);
    for (int i = 0; i < 128; i++) {
        if ((Y[i >> 3] >> (7 - (i & 7))) & 1)
            for (int k = 0; k < 16; k++) Z[k] ^= V[k];
        int lsb = V[15] & 1;
        for (int k = 15; k > 0; k--)
            V[k] = (uint8_t)((V[k] >> 1) | ((V[k - 1] & 1) << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xe1;   /* reduce by the GCM polynomial */
    }
    memcpy(Y, Z, 16);
}

/* Fold len bytes of data into the running GHASH state Y, one 16-byte (zero-
 * padded) block at a time. */
static void ghash_update(uint8_t Y[16], const uint8_t H[16],
                         const uint8_t *data, size_t len) {
    while (len) {
        uint8_t blk[16] = {0};
        size_t n = len < 16 ? len : 16;
        memcpy(blk, data, n);
        for (int k = 0; k < 16; k++) Y[k] ^= blk[k];
        ghash_mul(Y, H);
        data += n; len -= n;
    }
}

/* Full GHASH over AAD || ciphertext || len(AAD)||len(C) (bit lengths, 64-bit). */
static void ghash(const uint8_t H[16],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t *ct,  size_t ct_len,
                  uint8_t out[16]) {
    uint8_t Y[16] = {0};
    ghash_update(Y, H, aad, aad_len);
    ghash_update(Y, H, ct,  ct_len);
    uint8_t lb[16];
    put_be64(lb,     (uint64_t)aad_len * 8);
    put_be64(lb + 8, (uint64_t)ct_len  * 8);
    for (int k = 0; k < 16; k++) Y[k] ^= lb[k];
    ghash_mul(Y, H);
    memcpy(out, Y, 16);
}

/* Increment the low 32 bits of a counter block (mod 2^32), big-endian. */
static void inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--) {
        if (++ctr[i]) break;
    }
}

/* GCTR: XOR the keystream E_k(J0+1), E_k(J0+2), ... into in -> out. */
static void gctr(const struct gcm_ctx *g, const uint8_t j0[16],
                 const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t ctr[16], ks[16];
    memcpy(ctr, j0, 16);
    size_t off = 0;
    while (off < len) {
        inc32(ctr);
        g->enc(g->cctx, ctr, ks);
        size_t n = len - off < 16 ? len - off : 16;
        for (size_t k = 0; k < n; k++) out[off + k] = in[off + k] ^ ks[k];
        off += n;
    }
}

static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

void gcm_init(struct gcm_ctx *g, gcm_block_fn enc, const void *cctx) {
    g->enc = enc;
    g->cctx = cctx;
    uint8_t zero[16] = {0};
    enc(cctx, zero, g->H);   /* H = E_k(0^128) */
}

/* J0 for a 96-bit IV is IV || 0x00000001 (SP 800-38D §7.1, the TLS case). */
static void make_j0(const uint8_t iv[12], uint8_t j0[16]) {
    memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
}

void gcm_seal(const struct gcm_ctx *g, const uint8_t iv[12],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t pt_len,
              uint8_t *ct, uint8_t tag[16]) {
    uint8_t j0[16], ej0[16], s[16];
    make_j0(iv, j0);
    gctr(g, j0, pt, pt_len, ct);
    ghash(g->H, aad, aad_len, ct, pt_len, s);
    g->enc(g->cctx, j0, ej0);
    for (int k = 0; k < 16; k++) tag[k] = s[k] ^ ej0[k];
}

int gcm_open(const struct gcm_ctx *g, const uint8_t iv[12],
             const uint8_t *aad, size_t aad_len,
             const uint8_t *ct, size_t ct_len,
             const uint8_t tag[16], uint8_t *pt) {
    uint8_t j0[16], ej0[16], s[16], want[16];
    make_j0(iv, j0);
    ghash(g->H, aad, aad_len, ct, ct_len, s);
    g->enc(g->cctx, j0, ej0);
    for (int k = 0; k < 16; k++) want[k] = s[k] ^ ej0[k];
    if (!ct_eq(want, tag, 16)) return -1;   /* forged: release nothing */
    gctr(g, j0, ct, ct_len, pt);
    return 0;
}

static void aes128_enc_adapter(const void *cctx, const uint8_t in[16], uint8_t out[16]) {
    aes128_encrypt_block((const struct aes128_ctx *)cctx, in, out);
}

void aes128_gcm_init(struct aes128_gcm *c, const uint8_t key[16]) {
    aes128_init(&c->aes, key);
    gcm_init(&c->gcm, aes128_enc_adapter, &c->aes);
}
