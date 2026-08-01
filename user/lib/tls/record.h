#ifndef EMBK_TLS_RECORD_H
#define EMBK_TLS_RECORD_H
/* TLS 1.3 record layer (RFC 8446 §5). In 1.3 every record after the handshake
 * keys are installed is a TLSCiphertext with outer type application_data(23);
 * the real content type is the last non-zero byte of the AEAD-protected inner
 * plaintext. The per-record nonce is write_iv XOR the record sequence number
 * (§5.3); the AEAD's additional data is the 5-byte record header (§5.2). This
 * layer is AES-128-GCM-specific for now (the one T1 suite). */
#include <stdint.h>
#include <stddef.h>
#include "crypto/gcm.h"

/* TLS content types (RFC 8446 §5.1). */
#define TLS_CT_CHANGE_CIPHER_SPEC 20
#define TLS_CT_ALERT              21
#define TLS_CT_HANDSHAKE          22
#define TLS_CT_APPLICATION_DATA   23

#define TLS_RECORD_HEADER_LEN 5
#define TLS_TAG_LEN           16
#define TLS_RECORD_MAX_PLAINTEXT 16384          /* 2^14, RFC 8446 §5.1 */

/* One directional AEAD context: a key, an IV, and the running record sequence
 * number (both reset when traffic keys change -- handshake -> application). */
struct tls_keys {
    struct aes128_gcm aead;
    uint8_t  iv[12];
    uint64_t seq;
};

void tls_keys_init(struct tls_keys *k, const uint8_t key[16], const uint8_t iv[12]);

/* Seal `content` (of type `content_type`) into a TLSCiphertext record written to
 * `out`. Returns the total record length (header+ct+tag), or -1 if `out_cap` is
 * too small. Advances the sequence number. Encrypts in place -- no large buffer. */
int tls_record_seal(struct tls_keys *k, uint8_t content_type,
                    const uint8_t *content, size_t content_len,
                    uint8_t *out, size_t out_cap);

/* Open a complete TLSCiphertext record `rec` (header+ct+tag). On success writes
 * the inner content to `out`, sets *out_type to the real content type, returns
 * the content length, and advances the sequence number. Returns -1 on auth
 * failure or a malformed record (nothing is written on failure). `out_cap` must
 * hold the inner plaintext (content + the 1 type byte), i.e. one more than the
 * content -- decryption is in place into `out`. */
int tls_record_open(struct tls_keys *k, const uint8_t *rec, size_t rec_len,
                    uint8_t *out, size_t out_cap, uint8_t *out_type);

/* Build the per-record nonce = iv XOR seq (RFC 8446 §5.3). Exposed for testing. */
void tls_record_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12]);

#endif /* EMBK_TLS_RECORD_H */
