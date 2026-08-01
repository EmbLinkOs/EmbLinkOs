#ifndef EMBK_TLS_ASN1_H
#define EMBK_TLS_ASN1_H
/* A minimal DER (Distinguished Encoding Rules) reader -- just enough ASN.1 to
 * walk X.509 certificates (docs/TLS.md T3). DER is TLV: a tag byte, a length
 * (short form < 0x80, or long form 0x8n followed by n length bytes), then the
 * value. We only read; we never encode. Everything is bounds-checked against an
 * explicit end pointer, since certs come off the wire from an untrusted peer. */
#include <stdint.h>
#include <stddef.h>

/* Universal-class tags used in X.509. Context-specific constructed tags are
 * 0xA0|n, primitive 0x80|n -- compared numerically at the call site. */
#define DER_BOOLEAN      0x01
#define DER_INTEGER      0x02
#define DER_BIT_STRING   0x03
#define DER_OCTET_STRING 0x04
#define DER_NULL         0x05
#define DER_OID          0x06
#define DER_UTF8STRING   0x0c
#define DER_PRINTABLE    0x13
#define DER_IA5STRING    0x16
#define DER_UTCTIME      0x17
#define DER_GENTIME      0x18
#define DER_SEQUENCE     0x30
#define DER_SET          0x31

/* One parsed element. `val`/`len` are the contents; the whole element spans
 * [val - hdr, val + len), so `val - hdr` is where it started on the wire (needed
 * to hash a tbsCertificate exactly as received). */
struct der_tlv {
    uint8_t        tag;
    const uint8_t *val;
    size_t         len;
    size_t         hdr;
};

/* Parse the TLV at `p`, bounded by `end`. 0 on success, -1 if malformed or the
 * declared length runs past `end`. */
int der_parse(const uint8_t *p, const uint8_t *end, struct der_tlv *t);

/* One past the last content byte -- the start of the next sibling. */
static inline const uint8_t *der_end(const struct der_tlv *t) { return t->val + t->len; }
/* Start of the element on the wire (tag byte). */
static inline const uint8_t *der_raw(const struct der_tlv *t) { return t->val - t->hdr; }
/* Total encoded size (header + contents). */
static inline size_t der_raw_len(const struct der_tlv *t) { return t->hdr + t->len; }

/* True if `t` is an OID whose contents equal the raw `oid` bytes. */
int der_oid_eq(const struct der_tlv *t, const uint8_t *oid, size_t oid_len);

#endif /* EMBK_TLS_ASN1_H */
