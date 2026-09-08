/* user/audio/resample.c -- see resample.h. */
#include <math.h>
#include <string.h>

#include "resample.h"

/* The prototype filter, as a table of PHASES.
 *
 * sinc(x) windowed by a Hann window over the tap span. Sampled at 512 phases
 * so an arbitrary rate ratio can pick the nearest -- 512 phases puts the phase
 * quantisation error far below the 16-bit noise floor, which is the only
 * accuracy that matters at the output. Built once. */
#define PHASES 512
static float g_filt[PHASES][RESAMP_TAPS];
static int   g_ready;

static void build(void)
{
    const float half = RESAMP_TAPS / 2.0f;
    for (int p = 0; p < PHASES; p++) {
        float frac = (float)p / (float)PHASES;
        for (int t = 0; t < RESAMP_TAPS; t++) {
            /* Distance from this tap to the point being reconstructed. */
            float x = (float)t - half + 1.0f - frac;
            float s = (x == 0.0f) ? 1.0f
                                  : sinf((float)M_PI * x) / ((float)M_PI * x);
            /* Hann window: without it the truncated sinc rings, which is
             * audible as a whistle on transients. */
            float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI *
                                         ((float)t + 0.5f) / (float)RESAMP_TAPS);
            g_filt[p][t] = s * w;
        }
        /* Normalise each phase to unity gain. Without this the output level
         * wobbles with the phase -- a slow tremolo on a steady tone, which is
         * the kind of artefact that sounds like the recording rather than the
         * resampler. */
        float sum = 0;
        for (int t = 0; t < RESAMP_TAPS; t++) sum += g_filt[p][t];
        if (sum != 0.0f)
            for (int t = 0; t < RESAMP_TAPS; t++) g_filt[p][t] /= sum;
    }
    g_ready = 1;
}

void resample_init(Resampler *r, int in_rate, int out_rate, int channels)
{
    if (!g_ready) build();
    memset(r, 0, sizeof *r);
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->channels = channels < 1 ? 1 : (channels > RESAMP_CH_MAX ? RESAMP_CH_MAX : channels);
    /* How far to advance through the input per output sample. */
    r->step = out_rate ? ((uint64_t)in_rate << 32) / (uint64_t)out_rate : ((uint64_t)1 << 32);
    r->have = RESAMP_TAPS;          /* start with silence behind us */
}

int resample_run(Resampler *r, const int16_t *in, int in_frames,
                 int16_t *out, int out_cap)
{
    int ch = r->channels;
    int written = 0;

    for (int i = 0; i < in_frames; i++) {
        /* Slide one input frame into the history. A ring buffer would avoid
         * the shift, but the history is 32 floats and the shift is not what
         * costs -- the taps are. */
        for (int c = 0; c < ch; c++) {
            memmove(&r->hist[c][0], &r->hist[c][1],
                    (RESAMP_TAPS * 2 - 1) * sizeof(float));
            r->hist[c][RESAMP_TAPS * 2 - 1] = (float)in[i * ch + c] / 32768.0f;
        }

        /* Emit every output sample that now falls inside the history. */
        while (r->pos < ((uint64_t)1 << 32) && written < out_cap) {
            uint32_t frac = (uint32_t)(r->pos >> 23) & (PHASES - 1);
            const float *f = g_filt[frac];
            for (int c = 0; c < ch; c++) {
                float acc = 0;
                const float *h = &r->hist[c][RESAMP_TAPS];
                for (int t = 0; t < RESAMP_TAPS; t++) acc += h[t - RESAMP_TAPS] * f[t];
                int v = (int)(acc * 32768.0f + (acc >= 0 ? 0.5f : -0.5f));
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                out[written * ch + c] = (int16_t)v;
            }
            written++;
            r->pos += r->step;
        }
        if (r->pos >= ((uint64_t)1 << 32)) r->pos -= ((uint64_t)1 << 32);
        if (written >= out_cap) break;
    }
    return written;
}
