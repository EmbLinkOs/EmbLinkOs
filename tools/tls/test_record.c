/* Host test for the TLS 1.3 record layer (docs/TLS.md T2). Built on the already
 * RFC-verified T1 AES-128-GCM, this checks the TLS-specific framing: the per-
 * record nonce (RFC 8446 §5.3), the header-as-AAD, inner type append + strip,
 * sequence advance, and tamper detection. Wire-exactness against a real server's
 * records is proven at T2 integration (a server's Finished MAC verifying is the
 * end-to-end check); here we prove the record logic self-consistently. */
#include <stdio.h>
#include <string.h>
#include "record.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL %s\n", msg); fails++; } else printf("  ok   %s\n", msg); } while (0)

static const uint8_t KEY[16] = {0x3f,0xce,0x51,0x60,0x09,0xc2,0x17,0x27,0xd0,0xf2,0xe4,0xe8,0x6e,0xe4,0x03,0xbc};
static const uint8_t IV[12]  = {0x5d,0x31,0x3e,0xb2,0x67,0x12,0x76,0xee,0x13,0x00,0x0b,0x30};

int main(void) {
    /* §5.3 nonce = iv XOR seq, right-aligned. */
    printf("nonce (RFC 8446 5.3):\n");
    uint8_t n0[12], n1[12], n[12];
    tls_record_nonce(IV, 0, n0);
    CHECK(memcmp(n0, IV, 12) == 0, "seq 0 -> nonce == iv");
    tls_record_nonce(IV, 1, n1);
    uint8_t want1[12]; memcpy(want1, IV, 12); want1[11] ^= 1;
    CHECK(memcmp(n1, want1, 12) == 0, "seq 1 -> low byte flipped");
    tls_record_nonce(IV, 0x0102ULL, n);
    uint8_t want2[12]; memcpy(want2, IV, 12); want2[11] ^= 0x02; want2[10] ^= 0x01;
    CHECK(memcmp(n, want2, 12) == 0, "seq 0x0102 -> two bytes");

    /* Header byte-exactness: an empty alert record is header 17 03 03 00 11
     * (ct_len = 0 content + 1 type + 16 tag = 0x11). */
    printf("framing:\n");
    struct tls_keys tx; tls_keys_init(&tx, KEY, IV);
    uint8_t rec[2048];
    int rl = tls_record_seal(&tx, TLS_CT_ALERT, NULL, 0, rec, sizeof rec);
    CHECK(rl == 5 + 1 + 16, "empty record length = 22");
    CHECK(rec[0] == 0x17 && rec[1] == 0x03 && rec[2] == 0x03 && rec[3] == 0x00 && rec[4] == 0x11,
          "header = 17 03 03 00 11");

    /* Round-trip across lengths and a type-23 payload that ENDS IN ZEROS -- the
     * padding strip must not eat real trailing-zero content. */
    printf("round-trip:\n");
    const size_t lens[] = {0, 1, 63, 1000};
    for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
        size_t L = lens[li];
        uint8_t msg[1000], got[1024]; uint8_t typ;   /* got: content + type byte */
        for (size_t i = 0; i < L; i++) msg[i] = (uint8_t)(i * 7 + 1);
        if (L >= 3) { msg[L-1] = 0; msg[L-2] = 0; }   /* trailing zeros in content */
        struct tls_keys s, r; tls_keys_init(&s, KEY, IV); tls_keys_init(&r, KEY, IV);
        int rn = tls_record_seal(&s, TLS_CT_HANDSHAKE, msg, L, rec, sizeof rec);
        int cn = tls_record_open(&r, rec, rn, got, sizeof got, &typ);
        char lbl[48]; snprintf(lbl, sizeof lbl, "len %zu: open == seal, type ok", L);
        CHECK(cn == (int)L && typ == TLS_CT_HANDSHAKE && memcmp(got, msg, L) == 0, lbl);
    }

    /* Sequence: three records in a row; a receiver at the matching seq opens
     * each, but opening out of order (wrong nonce) fails. */
    printf("sequence + tamper:\n");
    struct tls_keys s, r; tls_keys_init(&s, KEY, IV); tls_keys_init(&r, KEY, IV);
    uint8_t r0[128], r1[128], got[128], typ;
    int l0 = tls_record_seal(&s, TLS_CT_APPLICATION_DATA, (const uint8_t*)"one", 3, r0, sizeof r0);
    int l1 = tls_record_seal(&s, TLS_CT_APPLICATION_DATA, (const uint8_t*)"two", 3, r1, sizeof r1);
    CHECK(tls_record_open(&r, r1, l1, got, sizeof got, &typ) < 0, "seq 0 cannot open the seq-1 record");
    /* r's seq is still 0 (open failed without advancing), so record 0 opens. */
    CHECK(tls_record_open(&r, r0, l0, got, sizeof got, &typ) == 3 && memcmp(got, "one", 3) == 0, "then seq 0 opens record 0");
    CHECK(tls_record_open(&r, r1, l1, got, sizeof got, &typ) == 3 && memcmp(got, "two", 3) == 0, "seq 1 opens record 1");

    /* Tamper: flip a ciphertext byte -> auth fails. */
    struct tls_keys s2, r2; tls_keys_init(&s2, KEY, IV); tls_keys_init(&r2, KEY, IV);
    int lt = tls_record_seal(&s2, TLS_CT_HANDSHAKE, (const uint8_t*)"hello", 5, rec, sizeof rec);
    rec[7] ^= 0x40;
    CHECK(tls_record_open(&r2, rec, lt, got, sizeof got, &typ) < 0, "flipped ciphertext byte rejected");
    rec[7] ^= 0x40; rec[4] ^= 0x01;   /* now corrupt the length in the AAD */
    CHECK(tls_record_open(&r2, rec, lt, got, sizeof got, &typ) < 0, "corrupt header length rejected");

    printf("\nTLS 1.3 record layer: %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
