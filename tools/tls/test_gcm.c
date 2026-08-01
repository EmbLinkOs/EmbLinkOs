/* Host test for AES-128-GCM (docs/TLS.md T1) against the canonical GCM vectors
 * (McGrew & Viega, "The Galois/Counter Mode of Operation", = NIST SP 800-38D).
 * Reuses our own AES (kernel/crypto/aes.c) through the kshim. */
#include <stdio.h>
#include <string.h>
#include "gcm.h"

static size_t unhex(const char *h, uint8_t *out) {
    size_t n = strlen(h) / 2;
    for (size_t i = 0; i < n; i++) { unsigned b; sscanf(h + 2 * i, "%2x", &b); out[i] = (uint8_t)b; }
    return n;
}

static int case_ok(const char *name, const char *kh, const char *ivh,
                   const char *ah, const char *ph, const char *ch, const char *th) {
    uint8_t key[16], iv[12], aad[64], pt[128], ct[128], tag[16];
    uint8_t gct[128], gtag[16], gpt[128];
    unhex(kh, key); unhex(ivh, iv);
    size_t al = unhex(ah, aad), pl = unhex(ph, pt), cl = unhex(ch, ct);
    unhex(th, tag);

    struct aes128_gcm c;
    aes128_gcm_init(&c, key);

    int ok = 1;
    /* seal: ct + tag must match the vector */
    aes128_gcm_seal(&c, iv, aad, al, pt, pl, gct, gtag);
    if (cl && memcmp(gct, ct, cl) != 0) { printf("  FAIL %s: ciphertext mismatch\n", name); ok = 0; }
    if (memcmp(gtag, tag, 16) != 0)     { printf("  FAIL %s: tag mismatch\n", name);        ok = 0; }

    /* open: recovers plaintext, returns 0 */
    int r = aes128_gcm_open(&c, iv, aad, al, ct, cl, tag, gpt);
    if (r != 0 || (pl && memcmp(gpt, pt, pl) != 0)) { printf("  FAIL %s: open\n", name); ok = 0; }

    /* forged tag must be rejected */
    uint8_t bad[16]; memcpy(bad, tag, 16); bad[0] ^= 0x80;
    if (aes128_gcm_open(&c, iv, aad, al, ct, cl, bad, gpt) == 0) {
        printf("  FAIL %s: forged tag accepted!\n", name); ok = 0;
    }

    if (ok) printf("  ok   %s (pt=%zu aad=%zu)\n", name, pl, al);
    return ok;
}

int main(void) {
    int ok = 1;

    /* TC1 -- empty plaintext, empty AAD, zero key/IV. */
    ok &= case_ok("GCM TC1",
        "00000000000000000000000000000000", "000000000000000000000000",
        "", "", "", "58e2fccefa7e3061367f1d57a4e7455a");

    /* TC3 -- 64-byte (4-block) plaintext, no AAD. */
    ok &= case_ok("GCM TC3",
        "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "",
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
        "4d5c2af327cd64a62cf35abd2ba6fab4");

    /* TC4 -- 60-byte (unaligned) plaintext WITH 20-byte AAD. */
    ok &= case_ok("GCM TC4",
        "feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
        "feedfacedeadbeeffeedfacedeadbeefabaddad2",
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
        "5bc94fbc3221a5db94fae95ae7121a47");

    printf("\nAES-128-GCM: %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
