/* user/audio/mp3/mp3dec.c -- decode an MP3 to raw PCM, for the host.
 *
 * Exists so the decoder can be compared against a reference sample by sample
 * rather than listened to. "It sounds right" cannot distinguish a correct
 * decoder from one with a wrong scalefactor exponent, and this repo has
 * already shipped a beep that was the right note and a fifth of the length.
 *
 *   mp3dec in.mp3 out.pcm      (s16le, interleaved, native channel count)
 */
#include <stdio.h>
#include <stdlib.h>

#include "mp3.h"

struct sink { FILE *f; long frames; int ch, hz; };

static void on_pcm(void *ctx, const int16_t *pcm, int samples, int channels,
                   int samplerate)
{
    struct sink *s = ctx;
    s->ch = channels;
    s->hz = samplerate;
    s->frames += samples;
    fwrite(pcm, sizeof(int16_t), (size_t)samples * channels, s->f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: mp3dec in.mp3 out.pcm\n"); return 2; }

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    rewind(in);
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(in);

    struct sink s = { fopen(argv[2], "wb"), 0, 0, 0 };
    if (!s.f) { perror(argv[2]); return 1; }

    static Mp3Decoder d;
    mp3_init(&d);
    long total = mp3_decode_all(&d, buf, (size_t)sz, on_pcm, &s);
    fclose(s.f);

    fprintf(stderr, "  %s: %ld samples, %d ch, %d Hz, %llu frames, %llu bad granules\n",
            argv[1], total, s.ch, s.hz,
            (unsigned long long)d.frames, (unsigned long long)d.granules_bad);
    free(buf);
    return total > 0 ? 0 : 1;
}
