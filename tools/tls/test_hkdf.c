/* Host test for HKDF-SHA256 (docs/TLS.md T1) against RFC 5869 test vectors.
 * Compiles the SAME crypto that runs on the OS -- kernel/crypto/{sha256,hmac}.c
 * plus user/lib/tls/crypto/hkdf.c -- with the kshim headers, and checks the
 * published PRK + OKM. Run on the dev host (fast, no boot). */
#include <stdio.h>
#include <string.h>
#include "hkdf.h"

static int hexeq(const uint8_t *got, const char *hex, const char *what) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned b; sscanf(hex + 2 * i, "%2x", &b);
        if (got[i] != (uint8_t)b) {
            printf("  FAIL %s at byte %zu: got %02x want %02x\n", what, i, got[i], b);
            return 0;
        }
    }
    printf("  ok   %s (%zu bytes)\n", what, n);
    return 1;
}

static int run_case(const char *name,
                    const uint8_t *ikm, size_t ikm_len,
                    const uint8_t *salt, size_t salt_len,
                    const uint8_t *info, size_t info_len,
                    size_t L, const char *prk_hex, const char *okm_hex) {
    printf("%s:\n", name);
    uint8_t prk[32], okm[128];
    hkdf_extract(salt, salt_len, ikm, ikm_len, prk);
    int ok = hexeq(prk, prk_hex, "PRK");
    if (hkdf_expand(prk, info, info_len, okm, L) != 0) { printf("  FAIL expand\n"); return 0; }
    ok &= hexeq(okm, okm_hex, "OKM");
    return ok;
}

int main(void) {
    int ok = 1;

    /* RFC 5869 Test Case 1 */
    uint8_t ikm1[22];  memset(ikm1, 0x0b, sizeof ikm1);
    uint8_t salt1[13]; for (int i = 0; i < 13; i++) salt1[i] = (uint8_t)i;
    uint8_t info1[10]; for (int i = 0; i < 10; i++) info1[i] = (uint8_t)(0xf0 + i);
    ok &= run_case("RFC5869 case 1", ikm1, 22, salt1, 13, info1, 10, 42,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    /* RFC 5869 Test Case 3 -- zero-length salt AND info (the salt-NULL path). */
    ok &= run_case("RFC5869 case 3", ikm1, 22, NULL, 0, NULL, 0, 42,
        "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8");

    printf("\nHKDF-SHA256: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
