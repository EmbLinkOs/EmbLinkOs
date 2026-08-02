#ifndef EMBK_TLS_CRYPTO_SELFTEST_H
#define EMBK_TLS_CRYPTO_SELFTEST_H
/* Runs the TLS 1.3 handshake-crypto RFC vectors (HKDF, AES-128-GCM, X25519) on
 * the metal. Returns 0 on success. Wired to `test tls crypto`. */
int tls_crypto_run_selftests(void);
#endif
