#ifndef EMBK_TLS_SHA512_H
#define EMBK_TLS_SHA512_H
/* SHA-512 and SHA-384 (FIPS 180-4). SHA-384 is SHA-512 with a different IV and
 * the output truncated to 48 bytes. Needed by T3: certificate chains mix hash
 * sizes (the Cloudflare WE1 intermediate is signed with ecdsa-with-SHA384), and
 * the TLS 1.3 signature schemes that pair with SHA-384/512 use these. New here
 * (the kernel only had SHA-256); could be lifted into kernel/crypto later. */
#include <stdint.h>
#include <stddef.h>

#define SHA384_DIGEST_SIZE 48
#define SHA512_DIGEST_SIZE 64
#define SHA512_BLOCK_SIZE  128

struct sha512_ctx {
    uint64_t h[8];
    uint64_t len_lo, len_hi;      /* message length in bytes (128-bit) */
    uint8_t  buf[SHA512_BLOCK_SIZE];
    size_t   buflen;
};

void sha512_init(struct sha512_ctx *c);
void sha384_init(struct sha512_ctx *c);
void sha512_update(struct sha512_ctx *c, const void *data, size_t len);
void sha512_final(struct sha512_ctx *c, uint8_t out[SHA512_DIGEST_SIZE]);   /* 64 */
void sha384_final(struct sha512_ctx *c, uint8_t out[SHA384_DIGEST_SIZE]);   /* 48 */

void sha512(const void *data, size_t len, uint8_t out[SHA512_DIGEST_SIZE]);
void sha384(const void *data, size_t len, uint8_t out[SHA384_DIGEST_SIZE]);

#endif /* EMBK_TLS_SHA512_H */
