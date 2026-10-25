#ifndef EMBK_CRYPTO_SHA1_H
#define EMBK_CRYPTO_SHA1_H
/* SHA-1 for WPA2 HMAC/PBKDF2 compatibility. Reused from user/git/sha1. */
#include <stddef.h>
#include <stdint.h>

struct sha1_ctx {
    uint32_t h[5];
    uint64_t len;          /* total bytes fed */
    uint8_t  buf[64];
    size_t   n;            /* bytes buffered */
};

void sha1_init(struct sha1_ctx *c);
void sha1_update(struct sha1_ctx *c, const void *data, size_t len);
void sha1_final(struct sha1_ctx *c, uint8_t out[20]);

/* One-shot. */
void sha1(const void *data, size_t len, uint8_t out[20]);

/* 20 raw bytes -> 40-char lowercase hex (out must hold 41). */
void sha1_hex(const uint8_t in[20], char out[41]);

#endif /* EMBK_CRYPTO_SHA1_H */
