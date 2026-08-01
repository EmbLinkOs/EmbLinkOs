#ifndef EMBK_TLS_X509_CERT_H
#define EMBK_TLS_X509_CERT_H
/* X.509 v3 certificate parsing (docs/TLS.md T3). Zero-copy: every field is a
 * pointer + length into the caller's DER buffer, which must outlive the struct.
 * We extract exactly what chain verification needs -- the EC public key, the
 * tbsCertificate bytes and their signature, issuer/subject names, validity, and
 * the SubjectAltName dNSNames -- and ignore the rest. EC keys only for now
 * (RSA is a later brick); a cert with a non-EC key parses with curve == 0. */
#include <stdint.h>
#include <stddef.h>

enum { X509_CURVE_NONE = 0, X509_CURVE_P256, X509_CURVE_P384 };
enum { X509_KEY_NONE = 0, X509_KEY_EC, X509_KEY_RSA };
enum { X509_SIG_NONE = 0,
       X509_SIG_ECDSA_SHA256, X509_SIG_ECDSA_SHA384,
       X509_SIG_RSA_SHA256, X509_SIG_RSA_SHA384, X509_SIG_RSA_SHA512 };

struct x509_cert {
    const uint8_t *raw;        size_t raw_len;
    const uint8_t *tbs;        size_t tbs_len;      /* signed bytes, hashed as-is */

    int            key_type;                         /* X509_KEY_EC or X509_KEY_RSA */
    int            curve;                            /* EC: X509_CURVE_* */
    const uint8_t *qx, *qy;    size_t coord_len;     /* EC: public key, affine */
    const uint8_t *rsa_n;      size_t rsa_n_len;     /* RSA: modulus + exponent */
    const uint8_t *rsa_e;      size_t rsa_e_len;

    int            sig_alg;                          /* X509_SIG_* on this cert */
    const uint8_t *sig_r;      size_t sig_r_len;     /* ECDSA: r,s (leading zero stripped) */
    const uint8_t *sig_s;      size_t sig_s_len;
    const uint8_t *sig_raw;    size_t sig_raw_len;   /* RSA: raw signature bytes */

    const uint8_t *issuer;     size_t issuer_len;    /* raw DER Name (for chain matching) */
    const uint8_t *subject;    size_t subject_len;

    const uint8_t *not_before; size_t not_before_len; uint8_t nb_tag;  /* UTCTime/GenTime */
    const uint8_t *not_after;  size_t not_after_len;  uint8_t na_tag;

    const uint8_t *san;        size_t san_len;       /* GeneralNames SEQUENCE, or NULL */
};

/* Parse a DER certificate. Returns 0 on success, -1 on malformed input. */
int x509_parse(const uint8_t *der, size_t len, struct x509_cert *out);

/* Chain-verification result. */
enum {
    X509_OK          =  0,
    X509_ERR_HOST    = -1,   /* leaf SAN does not cover the host */
    X509_ERR_EXPIRED = -2,   /* a cert is outside its validity window */
    X509_ERR_SIG     = -3,   /* a signature did not verify */
    X509_ERR_NAME    = -4,   /* issuer/subject linkage broken */
    X509_ERR_ANCHOR  = -5,   /* chain does not reach a trusted anchor */
};

/* Verify a server certificate chain: certs[0] is the leaf, certs[1..] the
 * intermediates in server order. Checks the leaf covers `host`, every cert is
 * within validity at `now_utc` ("yyyymmddhhmmssZ"-style 14 digits), each cert is
 * signed by the next with matching issuer/subject names, and the chain reaches a
 * bundled trust anchor (trust.h). Returns X509_OK or an X509_ERR_*. */
int x509_verify_chain(const struct x509_cert *certs, int n,
                      const char *host, const char *now_utc);

/* Does `host` match any dNSName in the cert's SAN (case-insensitive, one level
 * of leading "*." wildcard)? Returns 1 match, 0 no match. */
int x509_match_host(const struct x509_cert *c, const char *host);

/* Is `yyyymmddhhmmssZ` (14 ASCII digits + Z, UTC) within [notBefore,notAfter]?
 * Returns 1 valid, 0 expired/not-yet-valid/unparseable. */
int x509_check_validity(const struct x509_cert *c, const char *now_utc);

/* Verify `child`'s signature using `issuer`'s public key (hashes child->tbs with
 * the algorithm named in child->sig_alg and ECDSA-verifies). Returns 1 valid. */
int x509_verify_signed_by(const struct x509_cert *child, const struct x509_cert *issuer);

/* Same, but against a bundled trust anchor (EC or RSA key). */
struct trust_anchor;
int x509_verify_signed_by_anchor(const struct x509_cert *child, const struct trust_anchor *a);

/* Do two raw DER Names compare equal (byte-exact)? Used to link a cert to its
 * issuer in the chain. */
int x509_name_eq(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

#endif /* EMBK_TLS_X509_CERT_H */
