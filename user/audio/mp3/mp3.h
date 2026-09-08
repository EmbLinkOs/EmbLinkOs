/* user/audio/mp3/mp3.h -- the decoder, as a whole.
 *
 * Feed it bytes, take PCM. Everything below this line -- the frame walk, the
 * reservoir, the Huffman tables, the filterbank -- is an implementation detail
 * of these five calls.
 *
 * The decoder is a STATE MACHINE across frames, not a function of one frame,
 * and that is forced by the format rather than chosen: the bit reservoir means
 * a frame's data lives in earlier frames, the IMDCT overlaps each granule with
 * the previous one, and the synthesis filterbank carries a 1024-sample history
 * per channel. Decoding frame N in isolation is not possible even in
 * principle, which is why seeking costs a few frames of wrong audio.
 */
#ifndef _EMBLINK_MP3_H_
#define _EMBLINK_MP3_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "frame.h"
#include "reservoir.h"

#define MP3_MAX_SAMPLES 1152          /* frames per frame, per channel */

typedef struct {
    Mp3Reservoir res;

    /* Carried BETWEEN granules -- see the note above about state. */
    float overlap[2][32][18];         /* IMDCT tail, per channel per subband */
    float fifo[2][1024];              /* synthesis history, per channel      */
    int   fifo_pos[2];

    /* What the last decoded frame was. */
    int   samplerate, channels;
    bool  primed;                     /* have we seen a usable frame yet     */
    uint64_t frames, granules_bad;
} Mp3Decoder;

void mp3_init(Mp3Decoder *d);

/* Decode ONE frame beginning at `data` (which must start on a sync word).
 *
 * `pcm` receives interleaved 16-bit samples, `*out_samples` the number of
 * FRAMES (sample pairs) written. Returns the number of BYTES consumed, 0 if
 * more data is needed, or -1 if this is not a frame we can decode.
 *
 * A frame whose reservoir is cold -- which happens at the start of a file and
 * after a seek -- returns its byte length with *out_samples = 0. That is not
 * an error and must not be treated as one: the data was buffered, the next
 * frame will be fine, and refusing to advance would deadlock a player. */
int mp3_decode_frame(Mp3Decoder *d, const uint8_t *data, size_t size,
                     int16_t *pcm, int *out_samples);

/* Convenience for a whole buffer: find the first frame, decode until the end.
 * `on_pcm` is called once per decoded frame. */
typedef void (*Mp3PcmSink)(void *ctx, const int16_t *pcm, int samples,
                           int channels, int samplerate);
long mp3_decode_all(Mp3Decoder *d, const uint8_t *data, size_t size,
                    Mp3PcmSink on_pcm, void *ctx);

#endif /* _EMBLINK_MP3_H_ */
