/* DER reader (see asn1.h). Deliberately strict: multi-byte tags, indefinite
 * lengths, and lengths encoded in more than 4 bytes are all rejected -- none
 * occur in conformant X.509, and accepting them only widens the attack surface
 * on untrusted certificate bytes. */
#include "asn1.h"
#include <string.h>

int der_parse(const uint8_t *p, const uint8_t *end, struct der_tlv *t) {
    const uint8_t *start = p;
    if (p >= end) return -1;

    uint8_t tag = *p++;
    if ((tag & 0x1f) == 0x1f) return -1;          /* high-tag-number form: unused */
    if (p >= end) return -1;

    size_t len;
    uint8_t l0 = *p++;
    if (l0 < 0x80) {
        len = l0;                                  /* short form */
    } else {
        int nb = l0 & 0x7f;
        if (nb == 0 || nb > 4) return -1;          /* indefinite / oversized */
        if (end - p < nb) return -1;
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | *p++;
    }

    if ((size_t)(end - p) < len) return -1;        /* value runs past the buffer */

    t->tag = tag;
    t->val = p;
    t->len = len;
    t->hdr = (size_t)(p - start);
    return 0;
}

int der_oid_eq(const struct der_tlv *t, const uint8_t *oid, size_t oid_len) {
    return t->tag == DER_OID && t->len == oid_len && memcmp(t->val, oid, oid_len) == 0;
}
