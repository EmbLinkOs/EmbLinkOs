/* Host test for X.509 chain verification (docs/TLS.md T3) against a FROZEN real
 * Cloudflare chain (leaf + WE1 intermediate) verifying up to the bundled GTS
 * Root R4 anchor. `now` is taken from the leaf's own notBefore so the test stays
 * valid forever (the frozen leaf's real window will eventually pass). Signatures
 * are the durable part. */
#include <stdio.h>
#include <string.h>
#include "cert.h"
#include "trust.h"
#include "asn1.h"
#include "testdata_chain.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)

/* Normalize a cert's notBefore (UTCTime/GenTime) to 14 digits for `now`. */
static void now_from_nb(const struct x509_cert *c, char out[15]) {
    const uint8_t *t = c->not_before;
    if (c->nb_tag == DER_UTCTIME) { out[0]='2'; out[1]='0'; for (int i=0;i<12;i++) out[2+i]=(char)t[i]; }
    else { for (int i=0;i<14;i++) out[i]=(char)t[i]; }
    out[14]=0;
}

int main(void) {
    struct x509_cert leaf, we1, root;
    printf("parse frozen Cloudflare chain (leaf + WE1 + RSA-signed root):\n");
    CHECK(x509_parse(CHAIN_LEAF, sizeof CHAIN_LEAF, &leaf) == 0, "leaf parses");
    CHECK(x509_parse(CHAIN_WE1, sizeof CHAIN_WE1, &we1) == 0, "WE1 parses");
    CHECK(x509_parse(CHAIN_ROOT, sizeof CHAIN_ROOT, &root) == 0, "RSA-signed root parses (sig not ECDSA)");
    CHECK(root.sig_alg == X509_SIG_NONE, "root sig alg is non-ECDSA (tolerated)");
    CHECK(leaf.curve == X509_CURVE_P256, "leaf key is P-256");
    CHECK(we1.curve == X509_CURVE_P256, "WE1 key is P-256");
    CHECK(trust_find(we1.issuer, we1.issuer_len) != NULL, "WE1 issuer is the GTS Root R4 anchor");

    char now[15]; now_from_nb(&leaf, now);
    /* Verify the FULL 3-cert chain the server actually sends (the root is
     * redundant with our anchor, but must not break parsing/verification). */
    const struct x509_cert chain[3] = { leaf, we1, root };

    printf("chain verification (leaf <- WE1 <- GTS Root R4):\n");
    CHECK(x509_verify_chain(chain, 3, "cloudflare.com", now) == X509_OK, "valid 3-cert chain + host + anchor => OK");
    CHECK(x509_verify_chain(chain, 3, "attacker.example", now) == X509_ERR_HOST, "wrong host => ERR_HOST");
    CHECK(x509_verify_chain(chain, 3, "cloudflare.com", "20990101000000") == X509_ERR_EXPIRED, "future now => ERR_EXPIRED");

    /* Break the anchor link: a tampered WE1 tbs must fail its signature under the
     * GTS Root R4 key. */
    printf("tamper detection:\n");
    static uint8_t bad[sizeof CHAIN_WE1];
    memcpy(bad, CHAIN_WE1, sizeof CHAIN_WE1);
    bad[sizeof CHAIN_WE1 / 2] ^= 0x01;          /* flip a byte in the tbs region */
    struct x509_cert we1_bad;
    if (x509_parse(bad, sizeof bad, &we1_bad) == 0) {
        struct x509_cert ch2[2] = { leaf, we1_bad };
        CHECK(x509_verify_chain(ch2, 2, "cloudflare.com", now) != X509_OK, "tampered intermediate rejected");
    } else {
        printf("  ok   tampered intermediate rejected (failed to parse)\n");
    }

    printf("\nX.509 chain verification: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
