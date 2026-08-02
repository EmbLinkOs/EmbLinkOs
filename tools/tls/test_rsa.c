/* Host test for RSASSA-PKCS1-v1.5 verify (docs/TLS.md T3.5) against a Python-
 * generated RSA-2048/SHA-256 signature. Valid verifies; tampered sig/hash fail. */
#include <stdio.h>
#include <string.h>
#include "rsa.h"
#include "testdata_rsa.h"
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)
int main(void) {
    printf("RSA-2048 PKCS1-v1.5 / SHA-256:\n");
    CHECK(rsa_pkcs1_verify(RSA_N, sizeof RSA_N, RSA_E, sizeof RSA_E, RSA_HASH_SHA256,
          RSA_HASH, sizeof RSA_HASH, RSA_SIG, sizeof RSA_SIG) == 1, "valid signature verifies");
    { uint8_t bad[256]; memcpy(bad, RSA_SIG, 256); bad[128] ^= 0x01;
      CHECK(rsa_pkcs1_verify(RSA_N, sizeof RSA_N, RSA_E, sizeof RSA_E, RSA_HASH_SHA256,
            RSA_HASH, sizeof RSA_HASH, bad, 256) == 0, "tampered signature rejected"); }
    { uint8_t bh[32]; memcpy(bh, RSA_HASH, 32); bh[0] ^= 0x01;
      CHECK(rsa_pkcs1_verify(RSA_N, sizeof RSA_N, RSA_E, sizeof RSA_E, RSA_HASH_SHA256,
            bh, 32, RSA_SIG, sizeof RSA_SIG) == 0, "wrong hash rejected"); }
    { CHECK(rsa_pkcs1_verify(RSA_N, sizeof RSA_N, RSA_E, sizeof RSA_E, RSA_HASH_SHA384,
            RSA_HASH, 32, RSA_SIG, sizeof RSA_SIG) == 0, "wrong hash alg rejected"); }
    printf("\nRSA PKCS1-v1.5 verify: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
