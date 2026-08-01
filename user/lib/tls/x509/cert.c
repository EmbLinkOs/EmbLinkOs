/* X.509 certificate parsing + signature verification (see cert.h). */
#include "cert.h"
#include "asn1.h"
#include "trust.h"
#include "ecdsa.h"            /* -Iuser/lib/tls/crypto */
#include "sha512.h"           /* -Iuser/lib/tls/crypto */
#include "crypto/sha256.h"    /* kernel/crypto via -Ikernel */
#include <string.h>

/* OID contents (tag/length stripped). */
static const uint8_t OID_EC_PUBKEY[]   = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};        /* 1.2.840.10045.2.1 */
static const uint8_t OID_P256[]        = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};   /* prime256v1 */
static const uint8_t OID_P384[]        = {0x2b,0x81,0x04,0x00,0x22};                  /* secp384r1 */
static const uint8_t OID_ECDSA_SHA256[]= {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_SHA384[]= {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03};
static const uint8_t OID_SAN[]         = {0x55,0x1d,0x11};                            /* 2.5.29.17 */

#define OIDEQ(t, o) der_oid_eq((t), (o), sizeof(o))

/* Parse the DER SEQUENCE { r INTEGER, s INTEGER } of an ECDSA signature, storing
 * leading-zero-stripped r/s. */
static int parse_ecdsa_sig(const uint8_t *p, size_t len, struct x509_cert *c) {
    struct der_tlv seq;
    if (der_parse(p, p + len, &seq) || seq.tag != DER_SEQUENCE) return -1;
    struct der_tlv r, s;
    if (der_parse(seq.val, der_end(&seq), &r) || r.tag != DER_INTEGER) return -1;
    if (der_parse(der_end(&r), der_end(&seq), &s) || s.tag != DER_INTEGER) return -1;
    const uint8_t *rp = r.val; size_t rl = r.len;
    const uint8_t *sp = s.val; size_t sl = s.len;
    while (rl > 1 && rp[0] == 0) { rp++; rl--; }        /* strip DER sign byte */
    while (sl > 1 && sp[0] == 0) { sp++; sl--; }
    c->sig_r = rp; c->sig_r_len = rl;
    c->sig_s = sp; c->sig_s_len = sl;
    return 0;
}

/* SubjectPublicKeyInfo: SEQUENCE { AlgorithmIdentifier{ecPublicKey, curve}, BIT STRING }. */
static int parse_spki(const struct der_tlv *spki, struct x509_cert *c) {
    struct der_tlv alg, pk;
    if (der_parse(spki->val, der_end(spki), &alg) || alg.tag != DER_SEQUENCE) return -1;
    if (der_parse(der_end(&alg), der_end(spki), &pk) || pk.tag != DER_BIT_STRING) return -1;

    struct der_tlv alg_oid, curve_oid;
    if (der_parse(alg.val, der_end(&alg), &alg_oid) || !OIDEQ(&alg_oid, OID_EC_PUBKEY)) return -1;
    if (der_parse(der_end(&alg_oid), der_end(&alg), &curve_oid) || curve_oid.tag != DER_OID) return -1;
    if (OIDEQ(&curve_oid, OID_P256)) c->curve = X509_CURVE_P256;
    else if (OIDEQ(&curve_oid, OID_P384)) c->curve = X509_CURVE_P384;
    else return -1;

    /* BIT STRING: first content byte = unused-bit count (0), then 0x04||X||Y. */
    if (pk.len < 2 || pk.val[0] != 0 || pk.val[1] != 0x04) return -1;
    size_t point = pk.len - 2;
    if (point % 2) return -1;
    c->coord_len = point / 2;
    c->qx = pk.val + 2;
    c->qy = pk.val + 2 + c->coord_len;
    return 0;
}

/* Find SubjectAltName inside the extensions [3] and store its GeneralNames. */
static void parse_extensions(const struct der_tlv *exts_explicit, struct x509_cert *c) {
    struct der_tlv seq;
    if (der_parse(exts_explicit->val, der_end(exts_explicit), &seq) || seq.tag != DER_SEQUENCE) return;
    const uint8_t *p = seq.val, *e = der_end(&seq);
    while (p < e) {
        struct der_tlv ext;
        if (der_parse(p, e, &ext) || ext.tag != DER_SEQUENCE) return;
        p = der_end(&ext);
        struct der_tlv oid, cur;
        if (der_parse(ext.val, der_end(&ext), &oid)) continue;
        const uint8_t *q = der_end(&oid);
        /* optional critical BOOLEAN */
        if (der_parse(q, der_end(&ext), &cur) == 0 && cur.tag == DER_BOOLEAN) q = der_end(&cur);
        struct der_tlv val;
        if (der_parse(q, der_end(&ext), &val) || val.tag != DER_OCTET_STRING) continue;
        if (OIDEQ(&oid, OID_SAN)) {
            struct der_tlv gn;    /* extnValue wraps a GeneralNames SEQUENCE */
            if (der_parse(val.val, der_end(&val), &gn) == 0 && gn.tag == DER_SEQUENCE) {
                c->san = der_raw(&gn); c->san_len = der_raw_len(&gn);
            }
        }
    }
}

int x509_parse(const uint8_t *der, size_t len, struct x509_cert *out) {
    memset(out, 0, sizeof *out);
    out->raw = der; out->raw_len = len;

    struct der_tlv cert;
    if (der_parse(der, der + len, &cert) || cert.tag != DER_SEQUENCE) return -1;

    struct der_tlv tbs, sigalg, sigval;
    if (der_parse(cert.val, der_end(&cert), &tbs) || tbs.tag != DER_SEQUENCE) return -1;
    if (der_parse(der_end(&tbs), der_end(&cert), &sigalg) || sigalg.tag != DER_SEQUENCE) return -1;
    if (der_parse(der_end(&sigalg), der_end(&cert), &sigval) || sigval.tag != DER_BIT_STRING) return -1;
    out->tbs = der_raw(&tbs); out->tbs_len = der_raw_len(&tbs);

    /* outer signatureAlgorithm */
    struct der_tlv sa_oid;
    if (der_parse(sigalg.val, der_end(&sigalg), &sa_oid)) return -1;
    if (OIDEQ(&sa_oid, OID_ECDSA_SHA256)) out->sig_alg = X509_SIG_ECDSA_SHA256;
    else if (OIDEQ(&sa_oid, OID_ECDSA_SHA384)) out->sig_alg = X509_SIG_ECDSA_SHA384;
    else out->sig_alg = X509_SIG_NONE;

    /* signatureValue BIT STRING: unused-bits byte, then ECDSA-Sig-Value. */
    if (sigval.len < 1 || sigval.val[0] != 0) return -1;
    if (parse_ecdsa_sig(sigval.val + 1, sigval.len - 1, out)) return -1;

    /* Walk tbsCertificate fields in order. */
    const uint8_t *p = tbs.val, *e = der_end(&tbs);
    struct der_tlv f;
    if (der_parse(p, e, &f)) return -1;
    if (f.tag == 0xA0) p = der_end(&f), der_parse(p, e, &f);   /* skip version [0] */
    /* serialNumber */                       p = der_end(&f); if (der_parse(p, e, &f)) return -1;
    /* signature algid */                    p = der_end(&f); if (der_parse(p, e, &f) || f.tag != DER_SEQUENCE) return -1;
    /* issuer */                             out->issuer = der_raw(&f); out->issuer_len = der_raw_len(&f);
                                             p = der_end(&f); if (der_parse(p, e, &f) || f.tag != DER_SEQUENCE) return -1;
    /* validity */
    { struct der_tlv nb, na;
      if (der_parse(f.val, der_end(&f), &nb)) return -1;
      if (der_parse(der_end(&nb), der_end(&f), &na)) return -1;
      out->not_before = nb.val; out->not_before_len = nb.len; out->nb_tag = nb.tag;
      out->not_after  = na.val; out->not_after_len  = na.len; out->na_tag  = na.tag; }
    /* subject */                            p = der_end(&f); if (der_parse(p, e, &f) || f.tag != DER_SEQUENCE) return -1;
    out->subject = der_raw(&f); out->subject_len = der_raw_len(&f);
    /* subjectPublicKeyInfo */               p = der_end(&f); if (der_parse(p, e, &f) || f.tag != DER_SEQUENCE) return -1;
    if (parse_spki(&f, out)) return -1;

    /* optional [1] issuerUID, [2] subjectUID, [3] extensions */
    p = der_end(&f);
    while (der_parse(p, e, &f) == 0) {
        if (f.tag == 0xA3) { parse_extensions(&f, out); break; }
        p = der_end(&f);
    }
    return 0;
}

static int lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }

/* Match one dNSName pattern (pat/patlen) against host, one leading "*." wildcard. */
static int host_pat_eq(const uint8_t *pat, size_t patlen, const char *host) {
    if (patlen >= 2 && pat[0] == '*' && pat[1] == '.') {
        /* "*.example.com" matches "foo.example.com" but not "example.com" or
         * "a.b.example.com" (wildcard covers exactly one leftmost label). */
        const char *dot = strchr(host, '.');
        if (!dot) return 0;
        size_t suf = patlen - 1;                 /* ".example.com" */
        size_t hsuf = strlen(dot);
        if (hsuf != suf) return 0;
        for (size_t i = 0; i < suf; i++) if (lc(pat[1 + i]) != lc((unsigned char)dot[i])) return 0;
        return 1;
    }
    if (strlen(host) != patlen) return 0;
    for (size_t i = 0; i < patlen; i++) if (lc(pat[i]) != lc((unsigned char)host[i])) return 0;
    return 1;
}

int x509_match_host(const struct x509_cert *c, const char *host) {
    if (!c->san) return 0;
    struct der_tlv gn;
    if (der_parse(c->san, c->san + c->san_len, &gn) || gn.tag != DER_SEQUENCE) return 0;
    const uint8_t *p = gn.val, *e = der_end(&gn);
    while (p < e) {
        struct der_tlv name;
        if (der_parse(p, e, &name)) return 0;
        p = der_end(&name);
        if (name.tag == 0x82)                    /* [2] dNSName (IA5String, implicit) */
            if (host_pat_eq(name.val, name.len, host)) return 1;
    }
    return 0;
}

/* Normalize a UTCTime/GeneralizedTime to "yyyymmddhhmmss" (14 digits). */
static int time_to_14(const uint8_t *t, size_t len, uint8_t tag, char out[15]) {
    char buf[16]; size_t n = 0;
    if (tag == DER_UTCTIME) {                    /* YYMMDDHHMMSSZ -> add century */
        if (len < 12) return -1;
        int yy = (t[0]-'0')*10 + (t[1]-'0');
        const char *cc = (yy >= 50) ? "19" : "20";
        out[0]=cc[0]; out[1]=cc[1]; n = 2;
        for (int i = 0; i < 12 && n < 14; i++) out[n++] = (char)t[i];
    } else {                                     /* GeneralizedTime YYYYMMDDHHMMSSZ */
        if (len < 14) return -1;
        for (int i = 0; i < 14; i++) out[n++] = (char)t[i];
    }
    out[14] = 0;
    (void)buf;
    return 0;
}

int x509_check_validity(const struct x509_cert *c, const char *now_utc) {
    char nb[15], na[15];
    if (time_to_14(c->not_before, c->not_before_len, c->nb_tag, nb)) return 0;
    if (time_to_14(c->not_after,  c->not_after_len,  c->na_tag, na)) return 0;
    /* lexicographic compare of the 14-digit strings == chronological */
    if (strcmp(now_utc, nb) < 0) return 0;       /* not yet valid */
    if (strcmp(now_utc, na) > 0) return 0;       /* expired */
    return 1;
}

int x509_name_eq(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}

static int verify_sig(const struct x509_cert *child, int curve, const uint8_t *qx, const uint8_t *qy) {
    const struct ec_curve *ec;
    uint8_t hash[64]; size_t hlen;
    if (child->sig_alg == X509_SIG_ECDSA_SHA256) { sha256(child->tbs, child->tbs_len, hash); hlen = 32; }
    else if (child->sig_alg == X509_SIG_ECDSA_SHA384) { sha384(child->tbs, child->tbs_len, hash); hlen = 48; }
    else return 0;
    if (curve == X509_CURVE_P256) ec = ec_p256();
    else if (curve == X509_CURVE_P384) ec = ec_p384();
    else return 0;
    return ecdsa_verify(ec, qx, qy, hash, hlen,
                        child->sig_r, child->sig_r_len, child->sig_s, child->sig_s_len);
}

int x509_verify_signed_by(const struct x509_cert *child, const struct x509_cert *issuer) {
    return verify_sig(child, issuer->curve, issuer->qx, issuer->qy);
}
int x509_verify_sig_with_key(const struct x509_cert *child,
                             int issuer_curve, const uint8_t *iqx, const uint8_t *iqy) {
    return verify_sig(child, issuer_curve, iqx, iqy);
}

int x509_verify_chain(const struct x509_cert *certs, int n,
                      const char *host, const char *now_utc) {
    if (n < 1) return X509_ERR_ANCHOR;
    if (!x509_match_host(&certs[0], host)) return X509_ERR_HOST;

    /* Walk leaf -> up. Stop with success as soon as a cert's issuer is a trusted
     * anchor and that cert verifies under the anchor key; otherwise each cert
     * must be signed by the next one in the presented chain. */
    for (int i = 0; i < n; i++) {
        if (!x509_check_validity(&certs[i], now_utc)) return X509_ERR_EXPIRED;

        const struct trust_anchor *a = trust_find(certs[i].issuer, certs[i].issuer_len);
        if (a) {
            if (x509_verify_sig_with_key(&certs[i], a->curve, a->qx, a->qy)) return X509_OK;
            return X509_ERR_SIG;
        }
        if (i + 1 >= n) return X509_ERR_ANCHOR;          /* ran out; no anchor reached */
        if (!x509_name_eq(certs[i].issuer, certs[i].issuer_len,
                          certs[i + 1].subject, certs[i + 1].subject_len)) return X509_ERR_NAME;
        if (!x509_verify_signed_by(&certs[i], &certs[i + 1])) return X509_ERR_SIG;
    }
    return X509_ERR_ANCHOR;
}
