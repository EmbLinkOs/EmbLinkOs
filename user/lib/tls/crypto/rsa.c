/* RSASSA-PKCS1-v1.5 verify (RFC 8017 §8.2.2 / §9.2). */
#include "rsa.h"
#include "bignum.h"
#include <string.h>

/* DigestInfo DER prefixes (SEQUENCE{ AlgorithmIdentifier, OCTET STRING }) for
 * the SHA-2 family -- the fixed bytes that precede the raw hash in a v1.5
 * signature. */
static const uint8_t DI_SHA256[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const uint8_t DI_SHA384[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const uint8_t DI_SHA512[] = {
    0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

int rsa_pkcs1_verify(const uint8_t *n, size_t n_len,
                     const uint8_t *e, size_t e_len,
                     int hash_alg, const uint8_t *hash, size_t hash_len,
                     const uint8_t *sig, size_t sig_len) {
    while (n_len > 0 && n[0] == 0) { n++; n_len--; }     /* strip DER sign byte */
    while (e_len > 0 && e[0] == 0) { e++; e_len--; }
    if (n_len == 0 || n_len > BN_MAX_LIMBS * 8) return 0;
    if (sig_len == 0 || sig_len > n_len || e_len == 0) return 0;

    const uint8_t *di; size_t dilen;
    if (hash_alg == RSA_HASH_SHA256)      { di = DI_SHA256; dilen = sizeof DI_SHA256; }
    else if (hash_alg == RSA_HASH_SHA384) { di = DI_SHA384; dilen = sizeof DI_SHA384; }
    else if (hash_alg == RSA_HASH_SHA512) { di = DI_SHA512; dilen = sizeof DI_SHA512; }
    else return 0;

    struct mont mo;
    mont_init(&mo, n, n_len);

    /* m = sig^e mod n, with 1 <= sig < n. */
    bn s, sm, mm, m;
    bn_from_be(s, sig, sig_len, mo.n);
    if (bn_is_zero(s, mo.n) || bn_cmp(s, mo.m, mo.n) >= 0) return 0;
    mont_to(&mo, sm, s);
    mont_pow(&mo, mm, sm, e, e_len);
    mont_from(&mo, m, mm);

    uint8_t buf[BN_MAX_LIMBS * 8];
    bn_to_be(buf, m, mo.n);
    /* EM is the low n_len bytes (the value is < n, so any higher bytes are 0). */
    size_t emlen = n_len;
    const uint8_t *em = buf + (mo.n * 8 - emlen);

    /* Expected EM = 0x00 0x01 || 0xFF*PS || 0x00 || DigestInfo || hash. */
    size_t tlen = dilen + hash_len;
    if (emlen < tlen + 11) return 0;                     /* min 8 bytes of 0xFF */
    uint8_t exp[BN_MAX_LIMBS * 8];
    size_t p = 0;
    exp[p++] = 0x00; exp[p++] = 0x01;
    size_t pslen = emlen - 3 - tlen;
    memset(exp + p, 0xFF, pslen); p += pslen;
    exp[p++] = 0x00;
    memcpy(exp + p, di, dilen);  p += dilen;
    memcpy(exp + p, hash, hash_len);

    /* One difference bit fails the whole thing. */
    uint8_t diff = 0;
    for (size_t i = 0; i < emlen; i++) diff |= em[i] ^ exp[i];
    return diff == 0;
}
