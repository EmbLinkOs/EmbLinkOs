/* Bundled trust anchors. One for now -- GTS Root R4 (P-384) -- which Cloudflare,
 * Google, and a large fraction of the web chain to. Adding a root is adding an
 * entry here; the SDK/packaging story (docs) will later let /system carry a
 * curated, verified-boot-sealed set the user can grow. */
#include "trust.h"
#include "cert.h"
#include "roots.h"     /* GTS_R4_SUBJECT, GTS_R4_POINT (0x04||X||Y, P-384) */
#include <string.h>

/* GTS_R4_POINT is the uncompressed point 0x04 || X(48) || Y(48). */
static const struct trust_anchor ANCHORS[] = {
    {   /* GTS Root R4 -- EC P-384 (Cloudflare / Google) */
        GTS_R4_SUBJECT, sizeof GTS_R4_SUBJECT,
        X509_KEY_EC, X509_CURVE_P384,
        GTS_R4_POINT + 1, GTS_R4_POINT + 1 + 48, 48,
        NULL, 0, NULL, 0,
    },
    {   /* ISRG Root X1 -- RSA-4096 (Let's Encrypt, much of the web) */
        ISRG_X1_SUBJECT, sizeof ISRG_X1_SUBJECT,
        X509_KEY_RSA, X509_CURVE_NONE,
        NULL, NULL, 0,
        ISRG_X1_N, sizeof ISRG_X1_N, ISRG_X1_E, sizeof ISRG_X1_E,
    },
    {   /* GlobalSign Root R3 -- RSA-2048 (pypi.org / files.pythonhosted.org) */
        GS_R3_SUBJECT, sizeof GS_R3_SUBJECT,
        X509_KEY_RSA, X509_CURVE_NONE,
        NULL, NULL, 0,
        GS_R3_N, sizeof GS_R3_N, GS_R3_E, sizeof GS_R3_E,
    },
};
#define N_ANCHORS (int)(sizeof ANCHORS / sizeof ANCHORS[0])

const struct trust_anchor *trust_find(const uint8_t *issuer, size_t issuer_len) {
    for (int i = 0; i < N_ANCHORS; i++)
        if (ANCHORS[i].subject_len == issuer_len && memcmp(ANCHORS[i].subject, issuer, issuer_len) == 0)
            return &ANCHORS[i];
    return NULL;
}
