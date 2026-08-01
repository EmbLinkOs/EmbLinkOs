/* Host test for SHA-512 / SHA-384 (docs/TLS.md T3) against NIST known-answer
 * vectors ("" and "abc") plus OpenSSL-computed references for a 200-byte message
 * (crosses the 128-byte block boundary) and a 112-byte message (the padding edge
 * case where the length field forces an extra block). */
#include <stdio.h>
#include <string.h>
#include "sha512.h"

static int fails = 0;
static int check(const char *what, const uint8_t *got, size_t n, const char *hex) {
    uint8_t want[64];
    for (size_t i = 0; i < n; i++) { unsigned b; sscanf(hex + 2*i, "%2x", &b); want[i] = (uint8_t)b; }
    if (memcmp(got, want, n) != 0) { printf("  FAIL %s\n", what); fails++; return 0; }
    printf("  ok   %s\n", what); return 1;
}

int main(void) {
    uint8_t d[64];
    uint8_t msg200[200]; for (int i = 0; i < 200; i++) msg200[i] = (uint8_t)i;
    uint8_t a112[112];   memset(a112, 0x61, sizeof a112);

    printf("SHA-384:\n");
    sha384("", 0, d);
    check("empty", d, 48, "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b");
    sha384("abc", 3, d);
    check("abc", d, 48, "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
    sha384(msg200, 200, d);
    check("200-byte (2 blocks)", d, 48, "7ea4bb2534c67036f49de7beb5fe8a2478df04ff3fef40a9cd4923999a590e9912df1297217ce1a021aa2fb1013498b8");
    sha384(a112, 112, d);
    check("112-byte (padding edge)", d, 48, "187d4e07cb306103c69967bf544d0dfbe9042577599c73c330abc0cb64c61236d5ed565ee19119d8c31779a38f791fcd");

    printf("SHA-512:\n");
    sha512("", 0, d);
    check("empty", d, 64, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    sha512("abc", 3, d);
    check("abc", d, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    sha512(msg200, 200, d);
    check("200-byte (2 blocks)", d, 64, "986058e9895e2c2ab8f9e8cbdf801db12a44842a56a91d5a4e87b1fc98b293722c4664142e42c3c551ff898646268cd92b84ed230b8c94bed7798d4f27cd7465");

    printf("\nSHA-512/384: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
