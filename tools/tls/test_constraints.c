/* Host test for X.509 chain-constraint enforcement (docs/TLS.md T3 hardening):
 * Basic Constraints (CA:TRUE / pathLen), Key Usage (keyCertSign), and Extended
 * Key Usage (serverAuth). The headline is the FORGERY: the attacker cert is
 * validly signed, but by a non-CA leaf -- a verifier that only checked signatures
 * would accept it (the classic "any leaf can mint certs for any host" break). We
 * must refuse it with X509_ERR_USAGE. Also checks parsing on real certs. */
#include <stdio.h>
#include <string.h>
#include "cert.h"
#include "asn1.h"
#include "testdata_forge.h"
#include "testdata_chain.h"      /* the real Cloudflare chain (proper CAs) */

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)

static void now_from_nb(const struct x509_cert *c, char out[15]) {
    const uint8_t *t = c->not_before;
    if (c->nb_tag == DER_UTCTIME) { out[0]='2'; out[1]='0'; for (int i=0;i<12;i++) out[2+i]=(char)t[i]; }
    else { for (int i=0;i<14;i++) out[i]=(char)t[i]; }
    out[14]=0;
}

int main(void) {
    struct x509_cert att, leaf, cfleaf, we1;
    printf("parse:\n");
    CHECK(x509_parse(FORGE_ATTACKER, sizeof FORGE_ATTACKER, &att) == 0, "attacker parses");
    CHECK(x509_parse(FORGE_LEAF, sizeof FORGE_LEAF, &leaf) == 0, "victim leaf parses");
    CHECK(leaf.has_basic_constraints && !leaf.is_ca, "victim leaf: BasicConstraints CA:FALSE");
    CHECK(att.has_eku && att.eku_server_auth, "attacker leaf: EKU serverAuth present");

    /* Real Cloudflare certs: leaf is not a CA, WE1 is a CA with keyCertSign. */
    x509_parse(CHAIN_LEAF, sizeof CHAIN_LEAF, &cfleaf);
    x509_parse(CHAIN_WE1, sizeof CHAIN_WE1, &we1);
    CHECK(!cfleaf.is_ca, "real leaf (cloudflare.com): CA:FALSE");
    CHECK(we1.is_ca && (!we1.has_key_usage || (we1.key_usage & X509_KU_KEY_CERT_SIGN)),
          "real intermediate (WE1): CA:TRUE + keyCertSign");

    printf("forgery rejection (THE security property):\n");
    char now[15]; now_from_nb(&att, now);
    struct x509_cert chain[2] = { att, leaf };
    int r = x509_verify_chain(chain, 2, "evil.example", now);
    /* The signature att<-leaf is VALID; the chain must still be refused because
     * leaf is not a CA. Without enforcement this would be ERR_ANCHOR (-5) after
     * accepting the bad issuer; with enforcement it is ERR_USAGE (-6). */
    CHECK(r == X509_ERR_USAGE, "leaf-signed forgery refused with ERR_USAGE");
    CHECK(r != X509_OK, "forgery is NOT accepted");

    printf("\nX.509 constraint enforcement: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
