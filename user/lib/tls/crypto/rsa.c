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

/* MGF1(seed, len) with `hash` (RFC 8017 B.2.1): concat Hash(seed || i) for
 * counters 0,1,... until `len` bytes. */
static void mgf1(rsa_hash_fn hash, size_t hlen, const uint8_t *seed, size_t seedlen,
                 uint8_t *mask, size_t masklen) {
    uint8_t buf[BN_MAX_LIMBS * 8 + 4], t[64];
    memcpy(buf, seed, seedlen);
    size_t pos = 0;
    for (uint32_t counter = 0; pos < masklen; counter++) {
        buf[seedlen + 0] = (uint8_t)(counter >> 24);
        buf[seedlen + 1] = (uint8_t)(counter >> 16);
        buf[seedlen + 2] = (uint8_t)(counter >> 8);
        buf[seedlen + 3] = (uint8_t)counter;
        hash(buf, seedlen + 4, t);
        size_t take = masklen - pos < hlen ? masklen - pos : hlen;
        memcpy(mask + pos, t, take);
        pos += take;
    }
}

static size_t bitlen_be(const uint8_t *n, size_t n_len) {
    while (n_len > 0 && n[0] == 0) { n++; n_len--; }
    if (n_len == 0) return 0;
    size_t bits = (n_len - 1) * 8;
    uint8_t top = n[0];
    while (top) { bits++; top >>= 1; }
    return bits;
}

int rsa_pss_verify(const uint8_t *n, size_t n_len,
                   const uint8_t *e, size_t e_len,
                   rsa_hash_fn hash, size_t hlen,
                   const uint8_t *mhash,
                   const uint8_t *sig, size_t sig_len) {
    size_t modbits = bitlen_be(n, n_len);
    while (n_len > 0 && n[0] == 0) { n++; n_len--; }
    while (e_len > 0 && e[0] == 0) { e++; e_len--; }
    if (modbits == 0 || n_len > BN_MAX_LIMBS * 8) return 0;
    if (sig_len == 0 || sig_len > n_len || e_len == 0) return 0;

    struct mont mo;
    mont_init(&mo, n, n_len);
    bn s, sm, mm, m;
    bn_from_be(s, sig, sig_len, mo.n);
    if (bn_is_zero(s, mo.n) || bn_cmp(s, mo.m, mo.n) >= 0) return 0;
    mont_to(&mo, sm, s);
    mont_pow(&mo, mm, sm, e, e_len);
    mont_from(&mo, m, mm);

    uint8_t buf[BN_MAX_LIMBS * 8];
    bn_to_be(buf, m, mo.n);

    /* EM is the low emLen bytes, emLen = ceil((modBits-1)/8). */
    size_t embits = modbits - 1;
    size_t emlen = (embits + 7) / 8;
    if (emlen < hlen + 2) return 0;
    const uint8_t *em = buf + (mo.n * 8 - emlen);

    if (em[emlen - 1] != 0xbc) return 0;
    size_t dblen = emlen - hlen - 1;
    const uint8_t *maskedDB = em;
    const uint8_t *H = em + dblen;

    int zerobits = (int)(8 * emlen - embits);
    if (zerobits && (maskedDB[0] >> (8 - zerobits))) return 0;

    uint8_t DB[BN_MAX_LIMBS * 8];
    mgf1(hash, hlen, H, hlen, DB, dblen);
    for (size_t i = 0; i < dblen; i++) DB[i] ^= maskedDB[i];
    if (zerobits) DB[0] &= (uint8_t)(0xFF >> zerobits);

    /* DB = 0x00...00 || 0x01 || salt */
    size_t i = 0;
    while (i < dblen - 1 && DB[i] == 0) i++;
    if (DB[i] != 0x01) return 0;
    const uint8_t *salt = DB + i + 1;
    size_t slen = dblen - i - 1;

    /* H' = Hash(0x00*8 || mHash || salt); valid iff H' == H. */
    uint8_t mprime[8 + 64 + BN_MAX_LIMBS * 8], hp[64];
    memset(mprime, 0, 8);
    memcpy(mprime + 8, mhash, hlen);
    memcpy(mprime + 8 + hlen, salt, slen);
    hash(mprime, 8 + hlen + slen, hp);

    uint8_t diff = 0;
    for (size_t k = 0; k < hlen; k++) diff |= hp[k] ^ H[k];
    return diff == 0;
}
