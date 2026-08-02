/* HKDF-SHA256 (RFC 5869) -- Extract-then-Expand over HMAC-SHA256. The TLS 1.3
 * key schedule (RFC 8446 §7.1) is nothing but repeated HKDF-Expand-Label calls
 * on top of these two functions, so this is the first crypto brick of the TLS
 * campaign (docs/TLS.md, T1). It reuses our HMAC verbatim -- proof that the same
 * kernel crypto compiles and works in userspace. */
#include "hkdf.h"
#include "crypto/hmac.h"   /* hmac_sha256() -- reused from kernel/crypto */
#include <string.h>

void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm,  size_t ikm_len,
                  uint8_t prk[HKDF_HASH_LEN])
{
    uint8_t zero[HKDF_HASH_LEN] = {0};
    if (!salt || salt_len == 0) { salt = zero; salt_len = HKDF_HASH_LEN; }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);   /* PRK = HMAC(salt, IKM) */
}

/* T(0) = empty; T(i) = HMAC(PRK, T(i-1) | info | i); OKM = T(1)|T(2)|... [:L].
 * info is the TLS HkdfLabel, always small, so one bounded concat buffer is fine
 * (HMAC here is one-shot, not streaming). */
#define HKDF_INFO_MAX 512

int hkdf_expand(const uint8_t prk[HKDF_HASH_LEN],
                const uint8_t *info, size_t info_len,
                uint8_t *okm, size_t okm_len)
{
    if (okm_len > 255u * HKDF_HASH_LEN) return -1;
    if (info_len > HKDF_INFO_MAX)       return -1;

    uint8_t t[HKDF_HASH_LEN];
    uint8_t buf[HKDF_HASH_LEN + HKDF_INFO_MAX + 1];
    size_t  tlen = 0, done = 0;
    uint8_t ctr = 1;

    while (done < okm_len) {
        size_t n = 0;
        memcpy(buf + n, t, tlen);          n += tlen;        /* T(i-1) */
        if (info_len) memcpy(buf + n, info, info_len);
        n += info_len;                                        /* info   */
        buf[n++] = ctr;                                       /* i      */

        hmac_sha256(prk, HKDF_HASH_LEN, buf, n, t);
        tlen = HKDF_HASH_LEN;

        size_t take = (okm_len - done < HKDF_HASH_LEN) ? (okm_len - done) : HKDF_HASH_LEN;
        memcpy(okm + done, t, take);
        done += take;
        ctr++;
    }
    return 0;
}
