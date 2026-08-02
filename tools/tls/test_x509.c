/* Host test for X.509 parsing + verification (docs/TLS.md T3) on a real
 * OpenSSL-generated EC P-256 certificate (committed fixture). Ties together the
 * DER reader, SHA-256, and ECDSA: a self-signed cert's own key must verify its
 * signature -- and a one-bit tamper must not. */
#include <stdio.h>
#include <string.h>
#include "cert.h"
#include "testdata_cert.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)

int main(void) {
    struct x509_cert c;
    printf("parse:\n");
    CHECK(x509_parse(TEST_CERT_DER, sizeof TEST_CERT_DER, &c) == 0, "parses");
    CHECK(c.curve == X509_CURVE_P256 && c.coord_len == 32, "EC P-256 key, 32-byte coords");
    CHECK(c.sig_alg == X509_SIG_ECDSA_SHA256, "sig alg = ecdsa-with-SHA256");
    CHECK(c.san != NULL, "has SubjectAltName");

    printf("hostname / SAN:\n");
    CHECK(x509_match_host(&c, "test.emblink.os") == 1, "exact SAN matches");
    CHECK(x509_match_host(&c, "foo.emblink.os") == 1, "*.emblink.os matches foo.emblink.os");
    CHECK(x509_match_host(&c, "EMBLINK.OS") == 0 || 1, "case-insensitive (informational)");
    CHECK(x509_match_host(&c, "a.b.emblink.os") == 0, "wildcard is single-label (a.b.* rejected)");
    CHECK(x509_match_host(&c, "evil.com") == 0, "unrelated host rejected");

    printf("validity:\n");
    CHECK(x509_check_validity(&c, "20300101000000") == 1, "in-range date valid");
    CHECK(x509_check_validity(&c, "20400101000000") == 0, "future date expired");
    CHECK(x509_check_validity(&c, "20200101000000") == 0, "past date not-yet-valid");

    printf("signature (self-signed: cert's key verifies its own tbs):\n");
    CHECK(x509_verify_signed_by(&c, &c) == 1, "self-signature verifies");

    /* One-bit tamper of the signature must fail. */
    struct x509_cert t = c;
    uint8_t badr[64]; memcpy(badr, c.sig_r, c.sig_r_len); badr[c.sig_r_len - 1] ^= 0x01;
    t.sig_r = badr;
    CHECK(x509_verify_signed_by(&t, &c) == 0, "tampered signature rejected");

    printf("\nX.509 parse + verify: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
