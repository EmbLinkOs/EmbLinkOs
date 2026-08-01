/* Host test for the Montgomery bignum (docs/TLS.md T3) against Python-computed
 * references modulo the P-256 field prime: a*b mod p, a^-1 mod p, and reduction
 * of an over-length big-endian blob. Plus self-consistency (round-trips). */
#include <stdio.h>
#include <string.h>
#include "bignum.h"

static const uint8_t P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };

static int fails = 0;
static void bytes(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) { unsigned b; sscanf(hex + 2*i, "%2x", &b); out[i] = (uint8_t)b; }
}
static int eq(const char *what, const uint8_t *got, const char *hex, int n) {
    uint8_t want[64]; bytes(hex, want, n);
    if (memcmp(got, want, n) != 0) { printf("  FAIL %s\n", what); fails++; return 0; }
    printf("  ok   %s\n", what); return 1;
}

int main(void) {
    struct mont mo; mont_init(&mo, P256_P, 32);
    uint8_t A[32], B[32], out[32];
    bytes("01234567890abcdeffedcba09876543211122334455667788990011223344556", A, 32);
    bytes("9abcdef0112233445566778899aabbccddeeff00fedcba98765432100f0e0d0c", B, 32);

    bn a, b, am, bm, rm, r;
    bn_from_be(a, A, 32, mo.n);
    bn_from_be(b, B, 32, mo.n);

    /* a*b mod p */
    mont_to(&mo, am, a);
    mont_to(&mo, bm, b);
    mont_mul(&mo, rm, am, bm);
    mont_from(&mo, r, rm);
    bn_to_be(out, r, mo.n);
    printf("modmul / modinv:\n");
    eq("a*b mod p", out, "f39a21e95bf5aff98bec27d72a555498cc2ea35443b7b0da85888fcce7963b00", 32);

    /* a^-1 mod p */
    bn invm, inv;
    mont_inv(&mo, invm, am);
    mont_from(&mo, inv, invm);
    bn_to_be(out, inv, mo.n);
    eq("a^-1 mod p", out, "41b8eae810adb4fe72d37b16492e192c26a87732dce93edcff79a10146f3e1d3", 32);

    /* a * a^-1 == 1 (self-consistency) */
    bn onem, one;
    mont_mul(&mo, onem, am, invm);
    mont_from(&mo, one, onem);
    uint8_t ob[32]; bn_to_be(ob, one, mo.n);
    int is_one = ob[31] == 1; for (int i = 0; i < 31; i++) is_one &= (ob[i] == 0);
    if (!is_one) { printf("  FAIL a*a^-1 == 1\n"); fails++; } else printf("  ok   a*a^-1 == 1\n");

    /* reduce an over-length big-endian blob (48 bytes) mod p */
    printf("mont_load_be:\n");
    uint8_t blob[48]; for (int i = 0; i < 48; i++) blob[i] = (uint8_t)(i + 1);
    bn redm, red;
    mont_load_be(&mo, redm, blob, 48);
    mont_from(&mo, red, redm);
    bn_to_be(out, red, mo.n);
    eq("48-byte blob mod p", out, "18181817fefdfcfc131211100f0e0d0c0d0e0f102b2e3134373a3d4042444648", 32);

    /* mont_pow: a^65537 mod p (the RSA public-exponent shape). */
    printf("mont_pow:\n");
    static const uint8_t E[] = { 0x01, 0x00, 0x01 };   /* 65537, big-endian */
    bn powm, pw;
    mont_pow(&mo, powm, am, E, sizeof E);
    mont_from(&mo, pw, powm);
    bn_to_be(out, pw, mo.n);
    eq("a^65537 mod p", out, "42712ce0ffb70537be8266a4bc215c57ce2bcd0a72968f73065a32e0294a7bc3", 32);

    printf("\nMontgomery bignum: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
