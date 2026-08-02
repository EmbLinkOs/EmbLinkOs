/* Host test for the TLS 1.3 handshake message layer (docs/TLS.md T2): the
 * ClientHello builder and the ServerHello parser. Wire-exactness of a full
 * exchange is proven by the live handshake (tlstest on the OS); here we check
 * the encoders/decoders self-consistently and against hand-built messages. */
#include <stdio.h>
#include <string.h>
#include "handshake.h"

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL %s\n", m); fails++; } else printf("  ok   %s\n", m); } while (0)

/* Find `needle` (len n) in `hay` (len h). */
static int contains(const uint8_t *hay, size_t h, const uint8_t *needle, size_t n) {
    if (n > h) return 0;
    for (size_t i = 0; i + n <= h; i++) if (memcmp(hay + i, needle, n) == 0) return 1;
    return 0;
}

int main(void) {
    uint8_t crand[32], sid[32], pub[32];
    for (int i = 0; i < 32; i++) { crand[i] = (uint8_t)(i + 1); sid[i] = (uint8_t)(i + 0x40); pub[i] = (uint8_t)(i + 0x80); }

    printf("ClientHello:\n");
    uint8_t ch[1024];
    int n = tls_build_client_hello(ch, sizeof ch, crand, sid, pub, "cloudflare.com");
    CHECK(n > 0, "builds");
    CHECK(ch[0] == TLS_HS_CLIENT_HELLO, "type = client_hello(1)");
    size_t body = ((size_t)ch[1] << 16) | ((size_t)ch[2] << 8) | ch[3];
    CHECK(body == (size_t)n - 4, "uint24 length consistent");
    uint8_t suite[2] = { 0x13, 0x01 };
    CHECK(contains(ch, n, suite, 2), "offers TLS_AES_128_GCM_SHA256");
    CHECK(contains(ch, n, pub, 32), "embeds our x25519 key_share");
    CHECK(contains(ch, n, (const uint8_t *)"cloudflare.com", 14), "SNI present");

    /* Hand-build a minimal valid ServerHello with a known server key_share. */
    printf("ServerHello parse:\n");
    uint8_t spub[32]; for (int i = 0; i < 32; i++) spub[i] = (uint8_t)(0xA0 + i);
    uint8_t sh[256]; size_t p = 0;
    sh[p++] = TLS_HS_SERVER_HELLO; sh[p++] = 0; sh[p++] = 0; size_t lenpos = p++; /* u24 low byte */
    size_t bstart = p;
    sh[p++] = 0x03; sh[p++] = 0x03;                    /* legacy_version */
    for (int i = 0; i < 32; i++) sh[p++] = (uint8_t)(0x11 * (i & 7)); /* random (not HRR) */
    sh[p++] = 0;                                       /* empty session_id_echo */
    sh[p++] = 0x13; sh[p++] = 0x01;                    /* cipher suite */
    sh[p++] = 0;                                       /* compression */
    size_t extlp = p; p += 2;                          /* ext block len */
    /* supported_versions -> 0x0304 */
    sh[p++] = 0x00; sh[p++] = 0x2b; sh[p++] = 0x00; sh[p++] = 0x02; sh[p++] = 0x03; sh[p++] = 0x04;
    /* key_share x25519 */
    sh[p++] = 0x00; sh[p++] = 0x33; sh[p++] = 0x00; sh[p++] = 0x24;
    sh[p++] = 0x00; sh[p++] = 0x1d; sh[p++] = 0x00; sh[p++] = 0x20;
    memcpy(sh + p, spub, 32); p += 32;
    size_t extlen = p - (extlp + 2);
    sh[extlp] = (uint8_t)(extlen >> 8); sh[extlp + 1] = (uint8_t)extlen;
    sh[lenpos] = (uint8_t)(p - bstart);                /* body fits in one byte here */

    uint8_t got[32];
    int r = tls_parse_server_hello(sh, p, got);
    CHECK(r == 0 && memcmp(got, spub, 32) == 0, "extracts server key_share");

    /* HelloRetryRequest random must be reported as -2. */
    static const uint8_t hrr[32] = {
        0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
        0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };
    memcpy(sh + bstart + 2, hrr, 32);
    CHECK(tls_parse_server_hello(sh, p, got) == -2, "HelloRetryRequest detected");

    /* Wrong cipher suite must be rejected. */
    memcpy(sh + bstart + 2, (const uint8_t[]){0}, 1);  /* undo HRR (first byte) */
    for (int i = 0; i < 32; i++) sh[bstart + 2 + i] = (uint8_t)(0x11 * (i & 7));
    sh[bstart + 2 + 32 + 1] = 0x02;                    /* corrupt suite hi byte region */
    CHECK(tls_parse_server_hello(sh, p, got) != 0, "wrong suite rejected");

    printf("\nTLS 1.3 handshake messages: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
