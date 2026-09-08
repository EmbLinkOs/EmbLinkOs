/* user/audio/resample.h -- one sample rate into another, without the buzz.
 *
 * The speaker runs at 48000 and most music is 44100, so something has to
 * convert, and the naive conversions are audible. Dropping or repeating
 * samples puts a rasp on everything. Linear interpolation between neighbours
 * is better and still wrong in a specific way: it is a very poor low-pass, so
 * the images of the original spectrum fold back down as a metallic shimmer on
 * cymbals and sibilance -- the same aliasing the picture viewer's resampler
 * exists to avoid, in one dimension instead of two.
 *
 * The correct operation is a windowed sinc: reconstruct the continuous signal
 * the samples represent, then read it at the new spacing. Truncated to a few
 * taps and windowed so the truncation itself does not ring.
 *
 * Stateful ACROSS calls, deliberately. A stream arrives in blocks, and a
 * resampler that forgets its history between them puts a discontinuity at
 * every block boundary -- a click, 38 times a second.
 */
#ifndef _EMBLINK_AUDIO_RESAMPLE_H_
#define _EMBLINK_AUDIO_RESAMPLE_H_

#include <stdint.h>

#define RESAMP_TAPS   16          /* filter length, per output sample     */
#define RESAMP_CH_MAX  2

typedef struct {
    int      in_rate, out_rate, channels;
    /* Position in the INPUT stream, 32.32 fixed point. Fixed point rather
     * than float so the phase cannot drift over a long track. */
    uint64_t pos;
    uint64_t step;
    float    hist[RESAMP_CH_MAX][RESAMP_TAPS * 2];
    int      have;                /* samples of history held               */
} Resampler;

void resample_init(Resampler *r, int in_rate, int out_rate, int channels);

/* Convert `in_frames` interleaved frames, writing at most `out_cap` frames.
 * Returns frames written. Any input it could not use yet is retained. */
int  resample_run(Resampler *r, const int16_t *in, int in_frames,
                  int16_t *out, int out_cap);

#endif /* _EMBLINK_AUDIO_RESAMPLE_H_ */
