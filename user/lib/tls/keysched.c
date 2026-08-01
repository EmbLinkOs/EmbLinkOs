/* TLS 1.3 key schedule (RFC 8446 §7.1). Every secret in the handshake --
 * early/handshake/master secrets, the traffic secrets, and the per-record
 * write key/iv -- is derived here from HKDF-Expand-Label and Derive-Secret,
 * which are thin structural wrappers over our HKDF-SHA256 (T1). This file is
 * pure key derivation; it neither hashes the transcript nor touches the wire
 * (the caller feeds in Transcript-Hash values). */
#include "keysched.h"
#include "crypto/hkdf.h"
#include <string.h>

void tls_extract(const uint8_t *salt, const uint8_t *ikm, uint8_t prk[TLS_HASH_LEN]) {
    uint8_t zeros[TLS_HASH_LEN] = {0};
    const uint8_t *s = salt ? salt : zeros;   /* RFC's "0" == HashLen zero bytes */
    const uint8_t *k = ikm  ? ikm  : zeros;
    hkdf_extract(s, TLS_HASH_LEN, k, TLS_HASH_LEN, prk);
}

/* HkdfLabel (RFC 8446 §7.1):
 *   struct {
 *     uint16 length = out_len;
 *     opaque label<7..255>   = "tls13 " || label;   (1-byte length prefix)
 *     opaque context<0..255> = context;              (1-byte length prefix)
 *   }
 * The full label never exceeds 255 bytes and the contexts we pass are a 32-byte
 * hash or empty, so the fixed buffer below is ample. */
void tls_expand_label(const uint8_t secret[TLS_HASH_LEN], const char *label,
                      const uint8_t *ctx, size_t ctx_len,
                      uint8_t *out, size_t out_len) {
    static const char prefix[] = "tls13 ";
    size_t plen = sizeof(prefix) - 1;          /* 6 */
    size_t llen = strlen(label);
    uint8_t info[2 + 1 + (plen + 255) + 1 + 255];
    size_t n = 0;

    info[n++] = (uint8_t)(out_len >> 8);
    info[n++] = (uint8_t)(out_len & 0xff);
    info[n++] = (uint8_t)(plen + llen);        /* label length octet */
    memcpy(info + n, prefix, plen); n += plen;
    memcpy(info + n, label, llen);  n += llen;
    info[n++] = (uint8_t)ctx_len;              /* context length octet */
    if (ctx_len) { memcpy(info + n, ctx, ctx_len); n += ctx_len; }

    hkdf_expand(secret, info, n, out, out_len);
}

void tls_derive_secret(const uint8_t secret[TLS_HASH_LEN], const char *label,
                       const uint8_t transcript_hash[TLS_HASH_LEN],
                       uint8_t out[TLS_HASH_LEN]) {
    tls_expand_label(secret, label, transcript_hash, TLS_HASH_LEN, out, TLS_HASH_LEN);
}
