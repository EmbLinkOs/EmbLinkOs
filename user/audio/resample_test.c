/* user/audio/resample_test.c -- the resampler, measured.
 *
 * 44100 into 48000 is the conversion every music file needs on this machine,
 * and the ways of getting it wrong are all audible: sample dropping rasps,
 * linear interpolation shimmers on cymbals, an unnormalised filter puts a slow
 * tremolo on a steady tone. None of them fail; they just sound slightly bad,
 * which is why this measures instead of listening.
 *
 * A NOTE ON THE MEASUREMENT, because it was wrong first and said the filter
 * was 30x worse than it is. Comparing against an ideal sine requires knowing
 * the resampler's phase delay, and searching for it at INTEGER offsets leaves
 * up to half a sample of misalignment -- about 6 degrees at 1 kHz, which
 * appears as 1.6% "error" that is not distortion at all. Searching at
 * fractional phase gives 0.05%. This is the same mistake that made the MP3
 * decoder look broken when it matched the reference to one bit: a comparison
 * that is not aligned properly reports a fault in the thing being compared.
 *
 *   make test-resample
 */
#include <stdio.h>
#include <math.h>

#include "resample.h"

static int g_fail;

static void ok(const char *name, int cond, const char *detail)
{
    printf("  %-46s %s%s%s\n", name, cond ? "OK" : "FAIL",
           detail && *detail ? "  -- " : "", detail ? detail : "");
    if (!cond) g_fail++;
}

/* Best match against an ideal tone, searching phase finely. Returns RMS error
 * as a percentage of signal, and the peak seen. */
static double compare(const int16_t *out, int lo, int n, double hz, double rate,
                      double amp, double *out_peak)
{
    double best = 1e9;
    for (double k = 0; k < rate / hz; k += 0.02) {
        double e = 0, s = 0;
        for (int i = lo; i < lo + n; i++) {
            double ideal = amp * sin(2 * M_PI * hz * (i - k) / rate);
            double d = out[i] - ideal;
            e += d * d; s += ideal * ideal;
        }
        double pct = 100.0 * sqrt(e / n) / sqrt(s / n);
        if (pct < best) best = pct;
    }
    double peak = 0;
    for (int i = lo; i < lo + n; i++)
        if (fabs((double)out[i]) > peak) peak = fabs((double)out[i]);
    *out_peak = peak;
    return best;
}

int main(void)
{
    static int16_t in[48000 * 2], out[96000 * 2];
    char msg[160];
    printf("=== resampler\n");

    /* R1 -- the rate ratio itself. 44100 in must give 48000 out; a step
     * computed the wrong way round gives a file that plays at the wrong
     * speed, which sounds like a different recording rather than a bug. */
    for (int i = 0; i < 44100; i++)
        in[i] = (int16_t)(20000 * sin(2 * M_PI * 1000.0 * i / 44100.0));
    Resampler r;
    resample_init(&r, 44100, 48000, 1);
    int n = resample_run(&r, in, 44100, out, 96000);
    snprintf(msg, sizeof msg, "%d frames from 44100 (want 48000)", n);
    ok("R1 one second in, one second out", n >= 47990 && n <= 48010, msg);

    /* R2 -- the tone survives, at the right level and without distortion. */
    double peak = 0;
    double err = compare(out, 2000, 20000, 1000.0, 48000.0, 20000.0, &peak);
    snprintf(msg, sizeof msg, "err %.3f%%, droop %.2f%%", err,
             100.0 * (20000.0 - peak) / 20000.0);
    /* 0.2%: comfortably below anything audible, and far above the 0.05% a
     * correct filter achieves, so a real regression trips it. */
    ok("R2 a 1 kHz tone comes through clean", err < 0.2 && peak > 19800, msg);

    /* R3 -- DOWN as well as up. 48000 -> 44100 is the same filter run the
     * other way, and a resampler that only handles one direction is one that
     * has the ratio hardcoded somewhere it should not be. */
    for (int i = 0; i < 48000; i++)
        in[i] = (int16_t)(20000 * sin(2 * M_PI * 1000.0 * i / 48000.0));
    resample_init(&r, 48000, 44100, 1);
    n = resample_run(&r, in, 48000, out, 96000);
    err = compare(out, 2000, 20000, 1000.0, 44100.0, 20000.0, &peak);
    snprintf(msg, sizeof msg, "%d frames, err %.3f%%", n, err);
    ok("R3 downsampling works too", n >= 44090 && n <= 44110 && err < 0.2, msg);

    /* R4 -- STEREO stays separate. Interleaving is easy to get wrong in a way
     * that swaps or mixes the channels, and a mono test cannot see it. */
    for (int i = 0; i < 44100; i++) {
        in[i * 2 + 0] = (int16_t)(20000 * sin(2 * M_PI * 1000.0 * i / 44100.0));
        in[i * 2 + 1] = 0;
    }
    resample_init(&r, 44100, 48000, 2);
    n = resample_run(&r, in, 44100, out, 96000);
    double lsum = 0, rsum = 0;
    for (int i = 2000; i < 20000; i++) {
        lsum += fabs((double)out[i * 2 + 0]);
        rsum += fabs((double)out[i * 2 + 1]);
    }
    snprintf(msg, sizeof msg, "left %.0f, right %.0f (right must stay silent)",
             lsum / 18000, rsum / 18000);
    ok("R4 channels do not leak into each other",
       lsum / 18000 > 5000 && rsum / 18000 < 20, msg);

    /* R5 -- CONTINUITY across calls. A stream arrives in blocks; a resampler
     * that resets its history between them clicks at every boundary. Feeding
     * the same signal in small chunks must give the same answer. */
    for (int i = 0; i < 44100; i++)
        in[i] = (int16_t)(20000 * sin(2 * M_PI * 1000.0 * i / 44100.0));
    static int16_t chunked[96000];
    resample_init(&r, 44100, 48000, 1);
    int total = 0;
    for (int off = 0; off + 512 <= 44100; off += 512)
        total += resample_run(&r, in + off, 512, chunked + total, 96000 - total);
    resample_init(&r, 44100, 48000, 1);
    int whole = resample_run(&r, in, 44100, out, 96000);
    int cmp = total < whole ? total : whole;
    long worst = 0;
    for (int i = 0; i < cmp; i++) {
        long d = (long)chunked[i] - out[i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    snprintf(msg, sizeof msg, "%d vs %d frames, worst sample diff %ld",
             total, whole, worst);
    ok("R5 block boundaries leave no seam", worst <= 1, msg);

    printf("=== resampler: %s (%d failure%s)\n", g_fail ? "FAIL" : "OK",
           g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
