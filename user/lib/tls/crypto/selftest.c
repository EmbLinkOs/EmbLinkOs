/* On-OS selftest for the TLS 1.3 handshake crypto (docs/TLS.md T1), the same
 * RFC vectors the host tests (tools/tls/test_*.c) check -- but compiled INTO the
 * kernel and run on the metal, proving the primitives work in the real target,
 * not just under host gcc. Reached via `test tls crypto`.
 *
 * The two includes below are the crypto-reuse shim trick in action: under the
 * kernel build's -Ikernel they resolve to the kernel's own kstring.h/kprintf.h;
 * under the host kshim they resolve to <string.h> / a kprintf no-op -- so this
 * one file builds and runs in both worlds unchanged. */
#include "include/kstring.h"
#include "include/kprintf.h"
#include "hkdf.h"
#include "gcm.h"
#include "x25519.h"

static uint8_t hexnib(char c) {
    return (uint8_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
}
static size_t unhex(const char *h, uint8_t *out) {
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) out[n++] = (uint8_t)((hexnib(h[0]) << 4) | hexnib(h[1]));
    return n;
}
static int check(const char *what, const uint8_t *got, const char *hex, size_t n) {
    uint8_t want[128];
    unhex(hex, want);
    if (memcmp(got, want, n) != 0) {
        kprintf("CRYPTO: tls: FAIL %s\n", what);
        return 0;
    }
    return 1;
}

int tls_crypto_run_selftests(void) {
    int ok = 1;

    /* HKDF-SHA256, RFC 5869 Test Case 1. */
    {
        uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
        for (int i = 0; i < 22; i++) ikm[i]  = 0x0b;
        for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
        for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
        hkdf_extract(salt, 13, ikm, 22, prk);
        ok &= check("hkdf prk", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32);
        hkdf_expand(prk, info, 10, okm, 42);
        ok &= check("hkdf okm", okm,
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42);
    }

    /* AES-128-GCM, SP 800-38D TC4 -- 60-byte plaintext + 20-byte AAD (the
     * strongest single vector: unaligned length and associated data). */
    {
        uint8_t key[16], iv[12], aad[20], pt[60], ct[60], tag[16], gct[60], gtag[16], gpt[60];
        unhex("feffe9928665731c6d6a8f9467308308", key);
        unhex("cafebabefacedbaddecaf888", iv);
        unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad);
        unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt);
        unhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", ct);
        unhex("5bc94fbc3221a5db94fae95ae7121a47", tag);
        struct aes128_gcm g;
        aes128_gcm_init(&g, key);
        aes128_gcm_seal(&g, iv, aad, 20, pt, 60, gct, gtag);
        ok &= check("gcm ct", gct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", 60);
        ok &= check("gcm tag", gtag, "5bc94fbc3221a5db94fae95ae7121a47", 16);
        if (aes128_gcm_open(&g, iv, aad, 20, ct, 60, tag, gpt) != 0 || memcmp(gpt, pt, 60) != 0) {
            kprintf("CRYPTO: tls: FAIL gcm open\n"); ok = 0;
        }
        uint8_t bad[16]; memcpy(bad, tag, 16); bad[0] ^= 0x80;
        if (aes128_gcm_open(&g, iv, aad, 20, ct, 60, bad, gpt) == 0) {
            kprintf("CRYPTO: tls: FAIL gcm accepted forged tag\n"); ok = 0;
        }
    }

    /* X25519, RFC 7748 §6.1 -- a full ECDHE: two public keys from the base
     * point, then both sides agree on one shared secret. */
    {
        uint8_t apriv[32], bpriv[32], apub[32], bpub[32], sa[32], sb[32];
        unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", apriv);
        unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bpriv);
        x25519_base(apub, apriv);
        x25519_base(bpub, bpriv);
        ok &= check("x25519 apub", apub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
        ok &= check("x25519 bpub", bpub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
        x25519(sa, apriv, bpub);
        x25519(sb, bpriv, apub);
        ok &= check("x25519 shared", sa, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
        if (memcmp(sa, sb, 32) != 0) { kprintf("CRYPTO: tls: FAIL x25519 agreement\n"); ok = 0; }
    }

    kprintf("CRYPTO: tls (hkdf+aes128-gcm+x25519): %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : -1;
}
