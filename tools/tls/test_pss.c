/* Host test for RSA-PSS verify (docs/TLS.md T3.5), the TLS 1.3 RSA
 * CertificateVerify scheme, against a Python-generated PSS/SHA-256 signature. */
#include <stdio.h>
#include <string.h>
#include "rsa.h"
#include "crypto/sha256.h"
#include "testdata_pss.h"
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)
int main(void) {
    printf("RSA-2048 PSS / SHA-256 (MGF1-SHA256, salt=32):\n");
    CHECK(rsa_pss_verify(PSS_N, sizeof PSS_N, PSS_E, sizeof PSS_E, sha256, 32,
          PSS_MHASH, PSS_SIG, sizeof PSS_SIG) == 1, "valid PSS signature verifies");
    { uint8_t bad[256]; memcpy(bad, PSS_SIG, 256); bad[100] ^= 0x01;
      CHECK(rsa_pss_verify(PSS_N, sizeof PSS_N, PSS_E, sizeof PSS_E, sha256, 32,
            PSS_MHASH, bad, 256) == 0, "tampered signature rejected"); }
    { uint8_t bh[32]; memcpy(bh, PSS_MHASH, 32); bh[0] ^= 0x01;
      CHECK(rsa_pss_verify(PSS_N, sizeof PSS_N, PSS_E, sizeof PSS_E, sha256, 32,
            bh, PSS_SIG, sizeof PSS_SIG) == 0, "wrong message hash rejected"); }
    printf("\nRSA-PSS verify: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
