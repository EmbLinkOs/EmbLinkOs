/* Host test for RSA chain verification (docs/TLS.md T3.5): a frozen real Let's
 * Encrypt chain (leaf <- YR1 <- Root YR, all RSA-signed) verifying to the bundled
 * ISRG Root X1 RSA-4096 anchor -- RSA verify at every link. */
#include <stdio.h>
#include <string.h>
#include "cert.h"
#include "trust.h"
#include "asn1.h"
#include "testdata_lechain.h"
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)
static void now_from_nb(const struct x509_cert *c, char out[15]) {
    const uint8_t *t = c->not_before;
    if (c->nb_tag == DER_UTCTIME) { out[0]='2'; out[1]='0'; for (int i=0;i<12;i++) out[2+i]=(char)t[i]; }
    else { for (int i=0;i<14;i++) out[i]=(char)t[i]; }
    out[14]=0;
}
int main(void) {
    struct x509_cert leaf, yr1, root;
    printf("parse frozen Let's Encrypt (RSA) chain:\n");
    CHECK(x509_parse(LE_LEAF, sizeof LE_LEAF, &leaf) == 0, "leaf parses");
    CHECK(x509_parse(LE_YR1, sizeof LE_YR1, &yr1) == 0, "YR1 parses");
    CHECK(x509_parse(LE_ROOTYR, sizeof LE_ROOTYR, &root) == 0, "Root YR parses");
    CHECK(leaf.key_type == X509_KEY_RSA, "leaf key is RSA");
    CHECK(leaf.sig_alg == X509_SIG_RSA_SHA256, "leaf sig is RSA-SHA256");
    CHECK(trust_find(root.issuer, root.issuer_len) != NULL, "Root YR issuer is the ISRG Root X1 anchor");

    char now[15]; now_from_nb(&leaf, now);
    struct x509_cert chain[3] = { leaf, yr1, root };
    printf("chain verification (RSA at every link, -> ISRG Root X1):\n");
    CHECK(x509_verify_chain(chain, 3, LECHAIN_HOST, now) == X509_OK, "valid RSA chain + host + anchor => OK");
    CHECK(x509_verify_chain(chain, 3, "attacker.example", now) == X509_ERR_HOST, "wrong host => ERR_HOST");

    printf("tamper detection:\n");
    static uint8_t bad[sizeof LE_YR1];
    memcpy(bad, LE_YR1, sizeof LE_YR1); bad[sizeof LE_YR1/2] ^= 0x01;
    struct x509_cert yr1_bad;
    if (x509_parse(bad, sizeof bad, &yr1_bad) == 0) {
        struct x509_cert ch2[3] = { leaf, yr1_bad, root };
        CHECK(x509_verify_chain(ch2, 3, LECHAIN_HOST, now) != X509_OK, "tampered RSA intermediate rejected");
    } else printf("  ok   tampered intermediate rejected (failed to parse)\n");

    printf("\nRSA chain verification: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
