#ifndef EMBK_TLS_GCM_H
#define EMBK_TLS_GCM_H
/* AES-GCM (NIST SP 800-38D) -- the AEAD half of TLS_AES_128_GCM_SHA256. The
 * GHASH + CTR core is cipher-agnostic (a block-encrypt callback), so the same
 * code serves AES-128 now and AES-256-GCM (TLS_AES_256_GCM_SHA384) later. The
 * block cipher itself is our own AES (kernel/crypto/aes.c), reused in userspace
 * via the kshim -- see docs/TLS.md §1.3. */
#include <stdint.h>
#include <stddef.h>
#include "crypto/aes.h"

/* Block-cipher forward function: out = E_k(in), 128-bit block. */
typedef void (*gcm_block_fn)(const void *cctx, const uint8_t in[16], uint8_t out[16]);

struct gcm_ctx {
    uint8_t      H[16];   /* hash subkey = E_k(0^128) */
    gcm_block_fn enc;
    const void  *cctx;    /* opaque cipher context passed back to enc() */
};

void gcm_init(struct gcm_ctx *g, gcm_block_fn enc, const void *cctx);

/* AEAD seal: writes ct[0..pt_len) and the 16-byte tag. iv is the 96-bit
 * (12-byte) nonce TLS 1.3 always uses. ct may alias pt. */
void gcm_seal(const struct gcm_ctx *g, const uint8_t iv[12],
              const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t pt_len,
              uint8_t *ct, uint8_t tag[16]);

/* AEAD open: recomputes and verifies the tag (constant-time) BEFORE releasing
 * any plaintext. Returns 0 on success (pt[0..ct_len) written), -1 on auth
 * failure (pt left untouched -- a forged record yields nothing). */
int gcm_open(const struct gcm_ctx *g, const uint8_t iv[12],
             const uint8_t *aad, size_t aad_len,
             const uint8_t *ct, size_t ct_len,
             const uint8_t tag[16], uint8_t *pt);

/* Convenience wrapper: AES-128-GCM, the TLS_AES_128_GCM_SHA256 AEAD. */
struct aes128_gcm {
    struct aes128_ctx aes;
    struct gcm_ctx    gcm;
};
void aes128_gcm_init(struct aes128_gcm *c, const uint8_t key[16]);

#define aes128_gcm_seal(c, iv, aad, al, pt, pl, ct, tag) \
    gcm_seal(&(c)->gcm, (iv), (aad), (al), (pt), (pl), (ct), (tag))
#define aes128_gcm_open(c, iv, aad, al, ct, cl, tag, pt) \
    gcm_open(&(c)->gcm, (iv), (aad), (al), (ct), (cl), (tag), (pt))

#endif /* EMBK_TLS_GCM_H */
