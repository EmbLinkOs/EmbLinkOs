/* Host test for the TLS 1.3 key schedule (docs/TLS.md T2) against RFC 8448 §3
 * ("Simple 1-RTT Handshake") -- the canonical byte-exact trace. Walks the whole
 * extract/derive ladder (early -> handshake -> master) and derives the record
 * write key/iv from the handshake traffic secrets, checking every value against
 * the RFC. No network, no handshake-message transcripts needed: the "derived"
 * steps use SHA-256("") and the traffic-secret->key/iv steps take the secrets
 * as published inputs. */
#include <stdio.h>
#include <string.h>
#include "keysched.h"
#include "crypto/sha256.h"

static void unhex(const char *h, uint8_t *out) {
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) { unsigned b; sscanf(h + 2 * i, "%2x", &b); out[i] = (uint8_t)b; }
}
static int eq(const uint8_t *got, const char *hex, const char *what) {
    uint8_t want[64]; size_t n = strlen(hex) / 2;
    unhex(hex, want);
    if (memcmp(got, want, n) != 0) { printf("  FAIL %s\n", what); return 0; }
    printf("  ok   %s\n", what); return 1;
}

int main(void) {
    int ok = 1;
    uint8_t empty_hash[32], early[32], derived[32], hs[32], derived2[32], master[32];
    uint8_t ecdhe[32], secret[32], key[16], iv[12];

    /* Transcript-Hash("") = SHA-256("") -- also confirms our SHA-256 empty case. */
    sha256("", 0, empty_hash);
    ok &= eq(empty_hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256(\"\")");

    printf("RFC8448 secrets ladder:\n");
    /* Early Secret = HKDF-Extract(0, 0). */
    tls_extract(NULL, NULL, early);
    ok &= eq(early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a", "early secret");

    /* derived = Derive-Secret(Early, "derived", ""). */
    tls_derive_secret(early, "derived", empty_hash, derived);
    ok &= eq(derived, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba", "derived");

    /* Handshake Secret = HKDF-Extract(derived, ECDHE). */
    unhex("8bd4054fb55b9d63fdfbacf9f04b9f0d35e6d63f537563efd46272900f89492d", ecdhe);
    tls_extract(derived, ecdhe, hs);
    ok &= eq(hs, "1dc826e93606aa6fdc0aadc12f741b01046aa6b99f691ed221a9f0ca043fbeac", "handshake secret");

    /* derived2 = Derive-Secret(Handshake, "derived", ""); Master = Extract(derived2, 0). */
    tls_derive_secret(hs, "derived", empty_hash, derived2);
    tls_extract(derived2, NULL, master);
    ok &= eq(master, "18df06843d13a08bf2a449844c5f8a478001bc4d4c627984d5a41da8d0402919", "master secret");

    printf("RFC8448 record key/iv (HKDF-Expand-Label):\n");
    /* server handshake traffic secret -> write key + iv */
    unhex("b67b7d690cc16c4e75e54213cb2d37b4e9c912bcded9105d42befd59d391ad38", secret);
    tls_expand_label(secret, "key", NULL, 0, key, 16);
    tls_expand_label(secret, "iv",  NULL, 0, iv, 12);
    ok &= eq(key, "3fce516009c21727d0f2e4e86ee403bc", "server hs key");
    ok &= eq(iv,  "5d313eb2671276ee13000b30",         "server hs iv");

    /* client handshake traffic secret -> write key + iv */
    unhex("b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21", secret);
    tls_expand_label(secret, "key", NULL, 0, key, 16);
    tls_expand_label(secret, "iv",  NULL, 0, iv, 12);
    ok &= eq(key, "dbfaa693d1762c5b666af5d950258d01", "client hs key");
    ok &= eq(iv,  "5bd3c71b836e0b76bb73265f",         "client hs iv");

    printf("\nTLS 1.3 key schedule (RFC 8448): %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
