/* Host test for X25519 (docs/TLS.md T1) against RFC 7748 test vectors: the two
 * §5.2 scalar-mult KATs and the full §6.1 Diffie-Hellman exchange (both public
 * keys and the shared secret). */
#include <stdio.h>
#include <string.h>
#include "x25519.h"

static void unhex(const char *h, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) { unsigned b; sscanf(h + 2 * i, "%2x", &b); out[i] = (uint8_t)b; }
}
static int eq(const uint8_t got[32], const char *hex, const char *what) {
    uint8_t want[32]; unhex(hex, want);
    if (memcmp(got, want, 32) != 0) { printf("  FAIL %s\n", what); return 0; }
    printf("  ok   %s\n", what); return 1;
}

static int kat(const char *name, const char *scalar, const char *point, const char *expect) {
    uint8_t k[32], u[32], out[32];
    unhex(scalar, k); unhex(point, u);
    x25519(out, k, u);
    printf("%s:\n", name);
    return eq(out, expect, "scalar-mult");
}

int main(void) {
    int ok = 1;

    /* RFC 7748 §5.2 vector 1 */
    ok &= kat("RFC7748 v1",
        "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

    /* RFC 7748 §5.2 vector 2 */
    ok &= kat("RFC7748 v2",
        "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
        "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
        "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957");

    /* RFC 7748 §6.1 -- a real ECDHE handshake: derive both public keys from the
     * base point, then confirm both sides compute the SAME shared secret. */
    printf("RFC7748 6.1 DH:\n");
    uint8_t apriv[32], bpriv[32], apub[32], bpub[32], sa[32], sb[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", apriv);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bpriv);
    x25519_base(apub, apriv);
    x25519_base(bpub, bpriv);
    ok &= eq(apub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", "alice pub");
    ok &= eq(bpub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", "bob pub");
    x25519(sa, apriv, bpub);   /* alice: a * B */
    x25519(sb, bpriv, apub);   /* bob:   b * A */
    ok &= eq(sa, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", "shared (a*B)");
    if (memcmp(sa, sb, 32) != 0) { printf("  FAIL agreement a*B != b*A\n"); ok = 0; }
    else printf("  ok   agreement a*B == b*A\n");

    printf("\nX25519: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
