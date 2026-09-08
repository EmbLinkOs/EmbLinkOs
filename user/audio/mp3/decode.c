/* user/audio/mp3/decode.c -- one frame, all the way to PCM.
 *
 * The order of the stages is not a style choice; each one assumes the last has
 * happened. Requantise before reorder (the scalefactors are indexed the coded
 * way), reorder before stereo (stereo walks frequency bands), stereo before
 * the IMDCT (both channels must exist first), alias reduction inside the IMDCT
 * stage and before the transform, and the filterbank last.
 *
 * Get any pair the wrong way round and it still produces audio. That is the
 * whole difficulty of this file.
 */
#include <string.h>

#include "mp3.h"
#include "sideinfo.h"
#include "scalefac.h"
#include "huffman.h"
#include "spectrum.h"
#include "imdct.h"
#include "synth.h"

void mp3_init(Mp3Decoder *d)
{
    memset(d, 0, sizeof *d);
    mp3_res_reset(&d->res);
}

static int sr_index_of(int rate)
{
    return rate == 44100 ? 0 : rate == 48000 ? 1 : rate == 32000 ? 2 : -1;
}

static int16_t clip16(float v)
{
    /* The filterbank output is unbounded, and real files DO exceed full scale
     * -- an encoder is allowed to, because the decoder is expected to clip.
     * Wrapping instead of clipping turns a loud passage into a burst of noise,
     * which is the single most audible way to get this wrong. */
    int32_t s = (int32_t)(v * 32768.0f + (v >= 0 ? 0.5f : -0.5f));
    if (s >  32767) return  32767;
    if (s < -32768) return -32768;
    return (int16_t)s;
}

int mp3_decode_frame(Mp3Decoder *d, const uint8_t *data, size_t size,
                     int16_t *pcm, int *out_samples)
{
    *out_samples = 0;

    Mp3Header h;
    if (!mp3_parse_header(data, size, &h)) return -1;
    if ((size_t)h.frame_bytes > size) return 0;          /* need more input */
    if (h.version != MPEG_1) return -1;                  /* MPEG-2 not done */

    int sr_index = sr_index_of(h.samplerate);
    if (sr_index < 0) return -1;

    size_t si_off = 4 + (h.crc ? 2 : 0);
    Mp3SideInfo si;
    if (!mp3_parse_sideinfo(data + si_off, (int)(size - si_off), &h, &si))
        return -1;

    d->samplerate = h.samplerate;
    d->channels   = h.channels;

    const uint8_t *md = data + si_off + h.side_info_bytes;
    int mdlen = h.frame_bytes - (int)si_off - h.side_info_bytes;
    long startbit = mp3_res_add(&d->res, md, mdlen, si.main_data_begin);
    if (startbit < 0) {
        /* Cold reservoir. The bytes are buffered and the next frame will be
         * fine; returning the frame length with no samples is what lets a
         * player advance instead of stalling on a file's first frames. */
        return h.frame_bytes;
    }

    BitReader b;
    bits_init(&b, d->res.buf, d->res.len);
    bits_seek(&b, (size_t)startbit);

    Mp3Scalefac sf[2][2];
    float pcmf[2][1152];

    for (int gr = 0; gr < si.granules; gr++) {
        float xr[2][576];

        for (int ch = 0; ch < h.channels; ch++) {
            const Mp3Granule *g = &si.gr[gr][ch];
            size_t gend = bits_pos(&b) + g->part2_3_length;

            int32_t is[576];
            if (mp3_read_scalefactors(&b, &si, gr, ch, true, &sf[gr][ch],
                                      gr == 1 ? &sf[0][ch] : NULL) < 0) {
                d->granules_bad++;
                memset(xr[ch], 0, sizeof xr[ch]);
                bits_seek(&b, gend);
                continue;
            }
            if (mp3_huffman_granule(&b, gend, g, sr_index, is) < 0) {
                /* A corrupt granule becomes SILENCE, not noise, and decoding
                 * continues. One bad frame in a stream is normal; abandoning
                 * the file for it is not. */
                d->granules_bad++;
                memset(xr[ch], 0, sizeof xr[ch]);
                bits_seek(&b, gend);
                continue;
            }
            bits_seek(&b, gend);          /* exactly, whatever we consumed */

            mp3_requantise(is, g, &sf[gr][ch], sr_index, xr[ch]);
            mp3_reorder(xr[ch], g, sr_index);
        }

        if (h.channels == 2)
            mp3_stereo(xr[0], xr[1], &si.gr[gr][0], &sf[gr][1],
                       h.mode, h.mode_ext, sr_index);

        for (int ch = 0; ch < h.channels; ch++) {
            float sb[32][18];
            mp3_imdct_granule(xr[ch], &si.gr[gr][ch], d->overlap[ch], sb);
            mp3_synth_granule(sb, d->fifo[ch], pcmf[ch] + gr * 576);
        }
    }

    int n = 576 * si.granules;
    for (int i = 0; i < n; i++)
        for (int ch = 0; ch < h.channels; ch++)
            pcm[i * h.channels + ch] = clip16(pcmf[ch][i]);

    *out_samples = n;
    d->frames++;
    d->primed = true;
    return h.frame_bytes;
}

long mp3_decode_all(Mp3Decoder *d, const uint8_t *data, size_t size,
                    Mp3PcmSink on_pcm, void *ctx)
{
    size_t start = mp3_skip_id3(data, size);
    long first = mp3_find_sync(data, size, start);
    if (first < 0) return -1;

    static int16_t pcm[MP3_MAX_SAMPLES * 2];
    size_t p = (size_t)first;
    long total = 0;

    while (p + 4 <= size) {
        int n = 0;
        int used = mp3_decode_frame(d, data + p, size - p, pcm, &n);
        if (used <= 0) {
            /* Not a frame here. Hunt for the next one rather than stopping --
             * tags and padding appear mid-file in the wild. */
            long nxt = mp3_find_sync(data, size, p + 1);
            if (nxt < 0) break;
            p = (size_t)nxt;
            continue;
        }
        if (n > 0 && on_pcm)
            on_pcm(ctx, pcm, n, d->channels, d->samplerate);
        total += n;
        p += (size_t)used;
    }
    return total;
}
