/* user/audio/mp3/imdct.c -- see imdct.h. */
#include <math.h>
#include <string.h>

#include "imdct.h"

/* Alias-reduction coefficients, DERIVED rather than tabulated.
 *
 * The standard lists cs[] and ca[] to six decimals, but they are a formula
 * over eight numbers: cs = 1/sqrt(1+c^2), ca = c/sqrt(1+c^2). Only the eight
 * c values are arbitrary, so only they are written down here -- fewer typed
 * constants is fewer chances to typo one, and a derived table cannot drift
 * from its own definition. */
static const float k_ci[8] = {
    -0.6f, -0.535f, -0.33f, -0.185f, -0.095f, -0.041f, -0.0142f, -0.0037f
};
static float g_cs[8], g_ca[8];

/* Windows, also derived. Every one of them is a sine over some span. */
static float g_win[4][36];
static float g_cos36[36][18], g_cos12[12][6];
static bool  g_ready;

static void prepare(void)
{
    for (int i = 0; i < 8; i++) {
        float d = sqrtf(1.0f + k_ci[i] * k_ci[i]);
        g_cs[i] = 1.0f / d;
        g_ca[i] = k_ci[i] / d;
    }

    for (int i = 0; i < 36; i++) {
        g_win[BLOCK_LONG][i] = sinf((float)M_PI / 36.0f * ((float)i + 0.5f));

        /* START: a long window that narrows into the short ones that follow. */
        g_win[BLOCK_START][i] =
            i < 18 ? sinf((float)M_PI / 36.0f * ((float)i + 0.5f)) :
            i < 24 ? 1.0f :
            i < 30 ? sinf((float)M_PI / 12.0f * ((float)(i - 18) + 0.5f)) : 0.0f;

        /* STOP: the mirror image, widening back out. */
        g_win[BLOCK_STOP][i] =
            i <  6 ? 0.0f :
            i < 12 ? sinf((float)M_PI / 12.0f * ((float)(i - 6) + 0.5f)) :
            i < 18 ? 1.0f : sinf((float)M_PI / 36.0f * ((float)i + 0.5f));

        /* SHORT is applied per sub-window, so this slot holds the 12-point
         * window in its first 12 entries. */
        g_win[BLOCK_SHORT][i] =
            i < 12 ? sinf((float)M_PI / 12.0f * ((float)i + 0.5f)) : 0.0f;
    }

    /* The transform kernels, precomputed. A direct IMDCT is O(n^2) and there
     * are fast factorisations, but the cosines are the same every time, so the
     * table turns each output into 18 multiply-adds with no trigonometry. */
    for (int i = 0; i < 36; i++)
        for (int k = 0; k < 18; k++)
            g_cos36[i][k] = cosf((float)M_PI / 72.0f *
                                 (float)(2 * i + 1 + 18) * (float)(2 * k + 1));
    for (int i = 0; i < 12; i++)
        for (int k = 0; k < 6; k++)
            g_cos12[i][k] = cosf((float)M_PI / 24.0f *
                                 (float)(2 * i + 1 + 6) * (float)(2 * k + 1));
    g_ready = true;
}

/* Undo the encoder's alias cancellation across the 31 interior boundaries. */
static void antialias(float xr[576], const Mp3Granule *g)
{
    /* Short blocks were never alias-reduced. A mixed block WAS, but only in
     * its long region -- which is the lowest subband boundary only. */
    int subbands = 31;
    if (g->block_type == BLOCK_SHORT) {
        if (!g->mixed_block) return;
        subbands = 1;
    }

    for (int sb = 0; sb < subbands; sb++) {
        float *lo = xr + sb * 18 + 17;      /* last 8 of this subband, upward */
        float *hi = xr + sb * 18 + 18;      /* first 8 of the next            */
        for (int i = 0; i < 8; i++) {
            float a = lo[-i], b = hi[i];
            lo[-i] = a * g_cs[i] - b * g_ca[i];
            hi[i]  = b * g_cs[i] + a * g_ca[i];
        }
    }
}

void mp3_imdct_granule(const float xr[576], const Mp3Granule *g,
                       float overlap[32][18], float out[32][18])
{
    if (!g_ready) prepare();

    float work[576];
    memcpy(work, xr, sizeof work);
    antialias(work, g);

    for (int sb = 0; sb < 32; sb++) {
        const float *in = work + sb * 18;
        float raw[36];

        /* A mixed block's two lowest subbands use the LONG window even though
         * the granule is short -- that is what "mixed" means, and applying the
         * short path to them smears the bottom octave. */
        int bt = g->block_type;
        if (bt == BLOCK_SHORT && g->mixed_block && sb < 2) bt = BLOCK_LONG;

        if (bt == BLOCK_SHORT) {
            memset(raw, 0, sizeof raw);
            for (int win = 0; win < 3; win++) {
                float t[12];
                for (int i = 0; i < 12; i++) {
                    float s = 0;
                    for (int k = 0; k < 6; k++)
                        s += in[win + 3 * k] * g_cos12[i][k];
                    t[i] = s * g_win[BLOCK_SHORT][i];
                }
                /* The three short windows overlap each other by 6 inside the
                 * 36-sample slot, offset so the middle one is centred. */
                for (int i = 0; i < 12; i++)
                    raw[6 + win * 6 + i] += t[i];
            }
        } else {
            for (int i = 0; i < 36; i++) {
                float s = 0;
                for (int k = 0; k < 18; k++)
                    s += in[k] * g_cos36[i][k];
                raw[i] = s * g_win[bt][i];
            }
        }

        /* OVERLAP-ADD. The first half belongs to this granule's output, added
         * to the tail the previous granule left; the second half is saved to
         * be added to the next. This is why the MDCT can use a window twice
         * the hop size without the halves double-counting. */
        for (int i = 0; i < 18; i++) {
            out[sb][i]     = raw[i] + overlap[sb][i];
            overlap[sb][i] = raw[i + 18];
        }

        /* FREQUENCY INVERSION: every odd sample of every odd subband. */
        if (sb & 1)
            for (int i = 1; i < 18; i += 2) out[sb][i] = -out[sb][i];
    }
}
