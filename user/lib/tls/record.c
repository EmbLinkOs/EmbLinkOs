/* TLS 1.3 record layer (RFC 8446 §5). Framing plus per-record AES-128-GCM: the
 * inner plaintext is content || real_type (we emit no padding), the outer record
 * is header(23,0303,len) || AEAD(inner). The header doubles as the AEAD's
 * additional data, so a tampered length or type fails authentication. */
#include "record.h"
#include <string.h>

void tls_keys_init(struct tls_keys *k, const uint8_t key[16], const uint8_t iv[12]) {
    aes128_gcm_init(&k->aead, key);
    memcpy(k->iv, iv, 12);
    k->seq = 0;
}

/* nonce = write_iv XOR (seq, as a 64-bit big-endian value right-aligned in the
 * 12-byte IV) -- RFC 8446 §5.3. */
void tls_record_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* Write the 5-byte TLSCiphertext header for a body of `ct_len` bytes. */
static void put_header(uint8_t *h, size_t ct_len) {
    h[0] = TLS_CT_APPLICATION_DATA;
    h[1] = 0x03;   /* legacy_record_version = 0x0303 (TLS 1.2), always, in 1.3 */
    h[2] = 0x03;
    h[3] = (uint8_t)(ct_len >> 8);
    h[4] = (uint8_t)(ct_len & 0xff);
}

int tls_record_seal(struct tls_keys *k, uint8_t content_type,
                    const uint8_t *content, size_t content_len,
                    uint8_t *out, size_t out_cap) {
    if (content_len > TLS_RECORD_MAX_PLAINTEXT) return -1;
    size_t inner_len = content_len + 1;                 /* + content type */
    size_t ct_len    = inner_len + TLS_TAG_LEN;         /* AEAD output = ct || tag */
    size_t rec_len   = TLS_RECORD_HEADER_LEN + ct_len;
    if (rec_len > out_cap) return -1;

    put_header(out, ct_len);

    /* inner plaintext, laid out where the ciphertext will go, then encrypted in
     * place (GCM's CTR keystream XOR tolerates ct aliasing pt). */
    uint8_t *body = out + TLS_RECORD_HEADER_LEN;
    memcpy(body, content, content_len);
    body[content_len] = content_type;

    uint8_t nonce[12];
    tls_record_nonce(k->iv, k->seq, nonce);
    aes128_gcm_seal(&k->aead, nonce, out, TLS_RECORD_HEADER_LEN,
                    body, inner_len, body, body + inner_len);
    k->seq++;
    return (int)rec_len;
}

int tls_record_open(struct tls_keys *k, const uint8_t *rec, size_t rec_len,
                    uint8_t *out, size_t out_cap, uint8_t *out_type) {
    if (rec_len < TLS_RECORD_HEADER_LEN + TLS_TAG_LEN + 1) return -1;
    if (rec[0] != TLS_CT_APPLICATION_DATA) return -1;   /* 1.3: everything is wrapped */
    size_t ct_len = ((size_t)rec[3] << 8) | rec[4];
    if (ct_len + TLS_RECORD_HEADER_LEN != rec_len) return -1;

    size_t inner_len = ct_len - TLS_TAG_LEN;
    if (inner_len == 0 || inner_len > out_cap) return -1;

    const uint8_t *body = rec + TLS_RECORD_HEADER_LEN;
    const uint8_t *tag  = body + inner_len;
    uint8_t nonce[12];
    tls_record_nonce(k->iv, k->seq, nonce);

    /* Auth-then-decrypt into the caller's buffer; gcm_open writes nothing on a
     * bad tag, so `out` is untouched on failure. AAD = the 5-byte header. */
    if (aes128_gcm_open(&k->aead, nonce, rec, TLS_RECORD_HEADER_LEN,
                        body, inner_len, tag, out) != 0)
        return -1;
    k->seq++;

    /* Strip zero padding, then the trailing byte is the real content type. */
    size_t n = inner_len;
    while (n > 0 && out[n - 1] == 0) n--;
    if (n == 0) return -1;                 /* all padding, no type: malformed */
    *out_type = out[n - 1];
    return (int)(n - 1);                    /* content length (type byte removed) */
}
