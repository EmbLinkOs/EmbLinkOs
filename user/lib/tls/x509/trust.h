#ifndef EMBK_TLS_TRUST_H
#define EMBK_TLS_TRUST_H
/* Bundled trust anchors (docs/TLS.md T3, open sub-decision 5.1: a small curated
 * root set sealed under /system, not the whole Mozilla bundle). A chain is
 * trusted when it links up to one of these. Each anchor is a CA's Distinguished
 * Name plus its public key -- matched to a certificate whose issuer DN equals
 * the anchor's subject DN, then that certificate's signature is verified with
 * the anchor's key. */
#include <stdint.h>
#include <stddef.h>

struct trust_anchor {
    const uint8_t *subject;   size_t subject_len;   /* raw DER Name */
    int            key_type;                         /* X509_KEY_EC or X509_KEY_RSA */
    int            curve;                            /* EC anchor: X509_CURVE_* */
    const uint8_t *qx, *qy;   size_t coord_len;      /* EC anchor public key */
    const uint8_t *rsa_n;     size_t rsa_n_len;      /* RSA anchor modulus + exponent */
    const uint8_t *rsa_e;     size_t rsa_e_len;
};

/* Find the anchor whose subject DN equals `issuer`, or NULL. */
const struct trust_anchor *trust_find(const uint8_t *issuer, size_t issuer_len);

#endif /* EMBK_TLS_TRUST_H */
