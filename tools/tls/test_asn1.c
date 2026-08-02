/* Host test for the DER reader (docs/TLS.md T3). Hand-built TLVs exercise short
 * and long-form lengths, nested SEQUENCE iteration, OID matching, and bounds
 * rejection of truncated input. */
#include <stdio.h>
#include <string.h>
#include "asn1.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)

int main(void) {
    /* SEQUENCE { INTEGER 0x2A, OID 1.2.840.10045.4.3.2 (ecdsa-with-SHA256) } */
    static const uint8_t ecdsa_sha256[] = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };
    uint8_t seq[64]; size_t p = 0;
    seq[p++] = 0x30; size_t lp = p++;              /* SEQUENCE, short len */
    seq[p++] = DER_INTEGER; seq[p++] = 1; seq[p++] = 0x2a;
    seq[p++] = DER_OID; seq[p++] = sizeof ecdsa_sha256;
    memcpy(seq + p, ecdsa_sha256, sizeof ecdsa_sha256); p += sizeof ecdsa_sha256;
    seq[lp] = (uint8_t)(p - lp - 1);

    printf("short form + nesting:\n");
    struct der_tlv top;
    CHECK(der_parse(seq, seq + p, &top) == 0 && top.tag == DER_SEQUENCE, "outer SEQUENCE parses");
    CHECK(der_raw_len(&top) == p, "encoded length = whole buffer");

    const uint8_t *c = top.val, *ce = der_end(&top);
    struct der_tlv a, b;
    CHECK(der_parse(c, ce, &a) == 0 && a.tag == DER_INTEGER && a.len == 1 && a.val[0] == 0x2a, "child 1 INTEGER 0x2a");
    c = der_end(&a);
    CHECK(der_parse(c, ce, &b) == 0 && der_oid_eq(&b, ecdsa_sha256, sizeof ecdsa_sha256), "child 2 OID matches ecdsa-with-SHA256");
    CHECK(der_end(&b) == ce, "iteration consumes exactly the SEQUENCE");

    /* Long-form length: an OCTET STRING of 200 bytes -> length 0x81 0xC8. */
    printf("long form length:\n");
    uint8_t big[210]; size_t bp = 0;
    big[bp++] = DER_OCTET_STRING; big[bp++] = 0x81; big[bp++] = 200;
    for (int i = 0; i < 200; i++) big[bp++] = (uint8_t)i;
    struct der_tlv o;
    CHECK(der_parse(big, big + bp, &o) == 0 && o.tag == DER_OCTET_STRING && o.len == 200, "0x81 length parses to 200");
    CHECK(o.hdr == 3 && o.val[199] == 199, "header 3 bytes, contents intact");

    /* 0x82 two-byte length. */
    uint8_t two[6] = { DER_BIT_STRING, 0x82, 0x01, 0x00, 0xAA, 0xBB };
    struct der_tlv t2;
    /* declares 0x0100 = 256 bytes but only 2 present -> must reject. */
    CHECK(der_parse(two, two + sizeof two, &t2) == -1, "0x82 length past buffer rejected");

    printf("bounds:\n");
    uint8_t trunc[3] = { 0x30, 0x05, 0x02 };       /* SEQUENCE claims 5, has 1 */
    struct der_tlv tt;
    CHECK(der_parse(trunc, trunc + sizeof trunc, &tt) == -1, "truncated length rejected");
    CHECK(der_parse(trunc, trunc, &tt) == -1, "empty input rejected");

    printf("\nDER reader: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
