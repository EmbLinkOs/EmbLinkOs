#ifndef _EMBK_CRYPTO_AES_H
#define _EMBK_CRYPTO_AES_H

#include <stdint.h>

/*
 * AES (FIPS-197), single-block ECB primitive. EMBKFS v2 uses AES-256 (the
 * building block xts.h's AES-256-XTS wraps: two independent 256-bit keys, one
 * for data, one for tweak). TLS 1.3's mandatory TLS_AES_128_GCM_SHA256 suite
 * needs AES-128, so the round core is key-size-parametrized: the AddRoundKey/
 * SubBytes/ShiftRows/MixColumns transforms are shared verbatim between 128 and
 * 256; only the key schedule (Nk, Nr, and the AES-256-only extra SubWord)
 * differs. AES-192 is deliberately absent -- nothing here calls for it.
 */

#define AES_BLOCK_SIZE  16

#define AES256_KEY_SIZE 32
#define AES256_NR       14   /* number of rounds for a 256-bit key */

#define AES128_KEY_SIZE 16
#define AES128_NR       10   /* number of rounds for a 128-bit key */

struct aes256_ctx {
    /* Nr+1 round keys, 16 bytes (4 words) each. */
    uint8_t round_keys[(AES256_NR + 1) * AES_BLOCK_SIZE];
};

struct aes128_ctx {
    uint8_t round_keys[(AES128_NR + 1) * AES_BLOCK_SIZE];
};

void aes256_init(struct aes256_ctx *ctx, const uint8_t key[AES256_KEY_SIZE]);
void aes256_encrypt_block(const struct aes256_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);
void aes256_decrypt_block(const struct aes256_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);

/* AES-128 forward direction only -- GCM (AES-GCM AEAD) never runs the inverse
 * cipher, so no aes128_decrypt_block exists to get subtly wrong. */
void aes128_init(struct aes128_ctx *ctx, const uint8_t key[AES128_KEY_SIZE]);
void aes128_encrypt_block(const struct aes128_ctx *ctx, const uint8_t in[AES_BLOCK_SIZE], uint8_t out[AES_BLOCK_SIZE]);

/* FIPS-197 known-answer vectors (C.3 for AES-256, C.1 for AES-128), cross-
 * checked against Python's `cryptography` package (algorithms.AES + modes.ECB),
 * not just typed from memory. Also checks encrypt(decrypt(x)) == x round-trip. */
int aes256_run_selftests(void);

#endif /* _EMBK_CRYPTO_AES_H */
