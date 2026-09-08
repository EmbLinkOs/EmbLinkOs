/* user/audio/mp3/spectrum.c -- see spectrum.h. */
#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "spectrum.h"
#include "tables.h"

/* The pre-emphasis table. When a granule sets preflag, these are ADDED to the
 * scalefactors of the upper bands -- a cheap way to say "attenuate the top
 * end" without spending scalefactor bits on every band up there. */
static const uint8_t k_pretab[22] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0
};

/* |x|^(4/3), the inverse of the encoder's 3/4-power quantiser.
 *
 * Cached for the values that actually occur. Coefficients above 15 need
 * linbits and can reach 8206, but the overwhelming majority are 0, 1 or 2, so
 * a small table catches nearly every call and powf() handles the tail. Not a
 * micro-optimisation: this runs 576 times per granule, twice per frame, ~38
 * times a second, and powf on the hot path is what makes a decoder that cannot
 * keep up with real time. */
#define POW43_CACHE 256
static float g_pow43[POW43_CACHE];
static bool  g_pow43_ready;

static float pow43(int32_t v)
{
    int32_t a = v < 0 ? -v : v;
    float m;
    if (a < POW43_CACHE) {
        if (!g_pow43_ready) {
            for (int i = 0; i < POW43_CACHE; i++)
                g_pow43[i] = powf((float)i, 4.0f / 3.0f);
            g_pow43_ready = true;
        }
        m = g_pow43[a];
    } else {
        m = powf((float)a, 4.0f / 3.0f);
    }
    return v < 0 ? -m : m;
}

void mp3_requantise(const int32_t is[576], const Mp3Granule *g,
                    const Mp3Scalefac *sf, int sr_index, float xr[576])
{
    const Mp3SfBands *bands = &mp3_sf_bands[sr_index];

    /* The granule's overall level. 210 is the standard's offset; the /4 makes
     * global_gain a quarter-decibel-ish step. */
    float gain0 = 0.25f * ((float)g->global_gain - 210.0f);
    /* scalefac_scale picks how much each scalefactor step is worth: half a
     * step or a whole one. Getting this backwards makes everything the wrong
     * loudness in a way that still sounds like music. */
    float sf_mult = g->scalefac_scale ? 1.0f : 0.5f;

    memset(xr, 0, 576 * sizeof xr[0]);

    if (g->block_type == BLOCK_SHORT) {
        int sfb = 0, i = 0;

        /* A mixed block keeps LONG bands at the bottom, so the bottom of the
         * spectrum is scaled the long way even though the granule is short. */
        if (g->mixed_block) {
            for (sfb = 0; sfb < 8 && i < 576; sfb++) {
                float g_sfb = gain0 - sf_mult * (float)sf->l[sfb];
                float scale = exp2f(g_sfb);
                for (unsigned k = bands->l[sfb]; k < bands->l[sfb + 1] && i < 576; k++, i++)
                    xr[k] = pow43(is[k]) * scale;
            }
            sfb = 3;                    /* short bands resume here */
        }

        for (; sfb < 13; sfb++) {
            unsigned lo = bands->s[sfb], hi = bands->s[sfb + 1];
            if (lo >= 192) break;
            unsigned width = hi - lo;
            for (int win = 0; win < 3; win++) {
                /* subblock_gain attenuates one window of the three -- it is
                 * how the encoder puts the bits where the transient is. */
                float g_sfb = gain0
                            - 2.0f * (float)g->subblock_gain[win]
                            - sf_mult * (float)sf->s[sfb][win];
                float scale = exp2f(g_sfb);
                /* Window-major layout, which is why mp3_reorder exists. */
                unsigned base = lo * 3 + win * width;
                for (unsigned k = 0; k < width && base + k < 576; k++)
                    xr[base + k] = pow43(is[base + k]) * scale;
            }
        }
    } else {
        for (int sfb = 0; sfb < 22; sfb++) {
            unsigned lo = bands->l[sfb], hi = bands->l[sfb + 1];
            float pre = g->preflag ? (float)k_pretab[sfb] : 0.0f;
            float scale = exp2f(gain0 - sf_mult * ((float)sf->l[sfb] + pre));
            for (unsigned k = lo; k < hi && k < 576; k++)
                xr[k] = pow43(is[k]) * scale;
        }
    }
}

void mp3_reorder(float xr[576], const Mp3Granule *g, int sr_index)
{
    if (g->block_type != BLOCK_SHORT) return;

    const Mp3SfBands *bands = &mp3_sf_bands[sr_index];
    float tmp[576];
    memcpy(tmp, xr, sizeof tmp);

    /* Short blocks are coded window by window within each band; the filterbank
     * wants them frequency by frequency. A mixed block's long bottom is
     * already in frequency order and must be left where it is. */
    int start_sfb = g->mixed_block ? 3 : 0;

    for (int sfb = start_sfb; sfb < 13; sfb++) {
        unsigned lo = bands->s[sfb], hi = bands->s[sfb + 1];
        if (lo >= 192) break;
        unsigned width = hi - lo;
        for (unsigned k = 0; k < width; k++)
            for (int win = 0; win < 3; win++) {
                unsigned src = lo * 3 + (unsigned)win * width + k;
                unsigned dst = lo * 3 + k * 3 + (unsigned)win;
                if (src < 576 && dst < 576) xr[dst] = tmp[src];
            }
    }
}

/* Intensity-stereo position ratios: tan(i * pi/12), as a pair of gains that
 * sum in power to one. Derived rather than tabulated -- unlike the Huffman
 * codes, this one IS a formula in the standard. */
static void is_gains(int pos, float *l, float *r)
{
    if (pos == 0) { *l = 1.0f; *r = 1.0f; return; }
    float t = tanf((float)pos * (float)M_PI / 12.0f);
    *l = t / (1.0f + t);
    *r = 1.0f / (1.0f + t);
}

void mp3_stereo(float left[576], float right[576], const Mp3Granule *g,
                const Mp3Scalefac *sf_right, int mode, int mode_ext,
                int sr_index)
{
    if (mode != MODE_JOINT) return;

    bool ms  = (mode_ext & 2) != 0;
    bool ins = (mode_ext & 1) != 0;

    /* INTENSITY first, because it decides which coefficients MS may touch:
     * above the intensity boundary the right channel is not a signal at all,
     * it is a position, and mid/side arithmetic on it is meaningless. */
    if (ins) {
        const Mp3SfBands *bands = &mp3_sf_bands[sr_index];
        /* Find where the right channel stops carrying real coefficients. */
        unsigned bound = 576;
        for (unsigned i = 576; i > 0; i--) {
            if (right[i - 1] != 0.0f) { bound = i; break; }
            if (i == 1) bound = 0;
        }

        for (int sfb = 0; sfb < 22; sfb++) {
            unsigned lo = bands->l[sfb], hi = bands->l[sfb + 1];
            if (lo < bound) continue;              /* still real data here */
            int pos = sf_right->l[sfb];
            if (pos >= 7) continue;                /* 7 = "illegal", leave it */
            float gl, gr;
            is_gains(pos, &gl, &gr);
            for (unsigned k = lo; k < hi && k < 576; k++) {
                float v = left[k];
                left[k]  = v * gl;
                right[k] = v * gr;
            }
        }
    }

    if (ms) {
        /* Mid/side: the pair was stored as sum and difference. The 1/sqrt(2)
         * is what keeps total power the same through the transform. */
        const float inv_sqrt2 = 0.70710678f;
        unsigned limit = 576;
        for (unsigned i = 0; i < limit; i++) {
            float m = left[i], s = right[i];
            left[i]  = (m + s) * inv_sqrt2;
            right[i] = (m - s) * inv_sqrt2;
        }
    }
}
