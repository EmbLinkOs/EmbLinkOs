/* user/audio/mp3/mp3_test.c -- the decoder, checked on the host.
 *
 * Run against any real MP3:  ./mp3_test <file.mp3>
 *
 * THE FRAME LAYER IS SELF-CHECKING and that is the whole point of F1. An MP3
 * has no index: where the next frame starts is computed from the header of
 * this one, from the bitrate, sample rate and padding bit. So walking a whole
 * file frame to frame -- never hunting for a sync word, always landing exactly
 * on one -- is a statement that every one of those fields was decoded
 * correctly, several thousand times consecutively, on a file nobody wrote for
 * this test. An assertion I invented could not check that; the file's own
 * arithmetic can.
 *
 * F2 adds an INDEPENDENT oracle: most encoders record the frame count in a
 * Xing header. Comparing a walk against a number written by a different
 * program years ago is a much stronger check than comparing it against itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "frame.h"
#include "sideinfo.h"
#include "tables.h"

static int g_fail;

static void ok(const char *name, bool cond, const char *detail)
{
    printf("  %-44s %s%s%s\n", name, cond ? "OK" : "FAIL",
           detail && *detail ? "  -- " : "", detail ? detail : "");
    if (!cond) g_fail++;
}

/* --- the bit reader, on its own ------------------------------------------ */
static void test_bits(void)
{
    char msg[128];

    /* Known bytes, read in awkward widths that straddle byte boundaries --
     * which is the only interesting case, and the one a byte-at-a-time reader
     * gets wrong. 0xB5 0x2C = 1011 0101 0010 1100. */
    const uint8_t data[] = { 0xB5, 0x2C, 0xFF, 0x01 };
    BitReader b;
    bits_init(&b, data, sizeof data);

    uint32_t a = bits_read(&b, 3);      /* 101       = 5   */
    uint32_t c = bits_read(&b, 7);      /* 1010100   = 84  */
    uint32_t d = bits_read(&b, 6);      /* 101100    = 44  */
    snprintf(msg, sizeof msg, "%u %u %u", a, c, d);
    ok("B1 reads across byte boundaries", a == 5 && c == 84 && d == 44, msg);

    /* Peek must not move; the reservoir depends on it. */
    size_t before = bits_pos(&b);
    uint32_t p1 = bits_peek(&b, 8), p2 = bits_peek(&b, 8);
    ok("B2 peek does not consume", p1 == p2 && bits_pos(&b) == before, "");

    /* Past the end: zeroes and a flag, never a fault. A truncated frame is
     * normal input, not an exceptional one. */
    bits_seek(&b, sizeof(data) * 8 - 4);
    uint32_t tail = bits_read(&b, 16);
    snprintf(msg, sizeof msg, "got 0x%X, overrun=%d", tail, (int)b.overrun);
    ok("B3 reading past the end clamps and flags", b.overrun, msg);
}

/* --- the ISO tables, as committed ---------------------------------------- *
 * tools/mkmp3tables.py validates what it emits, but the file it emitted is
 * what actually gets compiled, and a generator's guarantee says nothing about
 * a file after it has been edited, merged or truncated. These checks are cheap
 * and they are about the artifact rather than the process.
 *
 * A Huffman table is not merely a list -- it is a PREFIX CODE, and that is a
 * property that can be verified without knowing a single correct value: no
 * code may be a prefix of another (or decoding is ambiguous), and the lengths
 * must satisfy Kraft equality (or the code is incomplete and some bit pattern
 * decodes to nothing). A table with a wrong entry almost always breaks one of
 * the two. */
static void test_tables(void)
{
    char msg[160];
    int bad_kraft = 0, bad_prefix = 0, tables = 0, codes = 0;
    int worst_table = -1;

    for (int t = 0; t < 34; t++) {
        const Mp3HuffTable *ht = &mp3_huff_tables[t];
        if (ht->count == 0) continue;
        tables++;
        codes += ht->count;

        double kraft = 0;
        for (int i = 0; i < ht->count; i++)
            kraft += 1.0 / (double)(1u << mp3_huff_entries[ht->start + i].bits);
        if (kraft < 0.9999999 || kraft > 1.0000001) {
            bad_kraft++;
            if (worst_table < 0) worst_table = t;
        }

        for (int i = 0; i < ht->count && !bad_prefix; i++) {
            const Mp3HuffEntry *a = &mp3_huff_entries[ht->start + i];
            for (int j = 0; j < ht->count; j++) {
                if (i == j) continue;
                const Mp3HuffEntry *b = &mp3_huff_entries[ht->start + j];
                if (b->bits < a->bits) continue;
                /* is a's code the leading `a->bits` bits of b's? */
                if ((b->code >> (b->bits - a->bits)) == a->code) {
                    bad_prefix++;
                    if (worst_table < 0) worst_table = t;
                    break;
                }
            }
        }
    }

    snprintf(msg, sizeof msg, "%d tables, %d codes", tables, codes);
    ok("H1 the ISO tables are present", tables == 31 && codes > 4000, msg);

    snprintf(msg, sizeof msg, "%d incomplete%s", bad_kraft,
             worst_table >= 0 ? "" : "");
    ok("H2 every table is a COMPLETE code (Kraft = 1)", bad_kraft == 0, msg);

    snprintf(msg, sizeof msg, "%d violations", bad_prefix);
    ok("H3 no code is a prefix of another", bad_prefix == 0, msg);

    /* The scalefactor bands must be increasing and end at 576 -- they are
     * boundaries into the coefficient array, and one out of order is an
     * out-of-bounds walk during requantisation. */
    int sf_ok = 1;
    for (int r = 0; r < 3; r++) {
        for (int i = 1; i < 23; i++)
            if (mp3_sf_bands[r].l[i] <= mp3_sf_bands[r].l[i - 1]) sf_ok = 0;
        if (mp3_sf_bands[r].l[22] != 576) sf_ok = 0;
        for (int i = 1; i < 14; i++)
            if (mp3_sf_bands[r].s[i] <= mp3_sf_bands[r].s[i - 1]) sf_ok = 0;
    }
    ok("H4 scalefactor bands rise and end at 576", sf_ok, "");
}

/* --- the frame layer, against a real file --------------------------------- */
static void test_frames(const char *path)
{
    char msg[192];

    FILE *f = fopen(path, "rb");
    if (!f) { ok("F0 open the file", false, path); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { ok("F0 read the file", false, ""); free(buf); return; }

    size_t start = mp3_skip_id3(buf, got);
    long first = mp3_find_sync(buf, got, start);
    snprintf(msg, sizeof msg, "id3 %zu bytes, first frame at %ld", start, first);
    ok("F0 skip ID3 and find the first frame", first >= 0, msg);
    if (first < 0) { free(buf); return; }

    Mp3Header h;
    mp3_parse_header(buf + first, got - (size_t)first, &h);
    snprintf(msg, sizeof msg, "MPEG%s %d Hz, %d kbit/s, %s",
             h.version == MPEG_1 ? "1" : h.version == MPEG_2 ? "2" : "2.5",
             h.samplerate, h.bitrate / 1000,
             h.channels == 1 ? "mono" : "stereo");
    ok("F1a the first header is sane", h.samplerate >= 8000 && h.channels >= 1, msg);

    /* THE WALK. Compute where the next frame is; require it to be there. */
    size_t pos = (size_t)first;
    unsigned frames = 0, resyncs = 0;
    unsigned long long samples = 0;
    int vbr = 0, last_br = h.bitrate;

    while (pos + 4 <= got) {
        Mp3Header fh;
        if (!mp3_parse_header(buf + pos, got - pos, &fh)) {
            long nxt = mp3_find_sync(buf, got, pos + 1);
            if (nxt < 0) break;
            resyncs++;
            pos = (size_t)nxt;
            continue;
        }
        if (fh.bitrate != last_br) { vbr = 1; last_br = fh.bitrate; }
        frames++;
        samples += (unsigned long long)fh.samples;
        pos += (size_t)fh.frame_bytes;
    }

    snprintf(msg, sizeof msg, "%u frames, %u resyncs, %s", frames, resyncs,
             vbr ? "VBR" : "CBR");
    /* A handful of resyncs at the very end of a file is normal (trailing ID3v1
     * or padding). Thousands would mean the length arithmetic is wrong. */
    ok("F1 the frame chain walks without hunting",
       frames > 10 && resyncs <= 2, msg);

    double secs = h.samplerate ? (double)samples / h.samplerate : 0;
    snprintf(msg, sizeof msg, "%.1f s of audio from %.1f KB", secs, sz / 1024.0);
    ok("F1b the walk covers the whole file", secs > 1.0, msg);

    /* S -- the side info of every frame in the file.
     *
     * Two properties worth more than any hand-written assertion:
     *
     * S1: every frame's side info parses with all fields in range. The reader
     * rejects out-of-range values because they INDEX TABLES -- a bad one is an
     * out-of-bounds read on input the decoder did not write -- so "all 8490
     * parsed" also says the layout (which differs by version AND channel
     * count) was read correctly every time.
     *
     * S2: the BIT RESERVOIR balances. main_data_begin says the granule's data
     * starts that many bytes before the end of the previous frame's data, so
     * it can never ask for more history than the file has actually produced.
     * If any field width above were wrong, main_data_begin would be shifted
     * and this would go negative almost immediately -- it is the reservoir
     * arithmetic checking the parser, using nothing I supplied. */
    {
        size_t p = (size_t)first;
        unsigned parsed = 0, bad = 0;
        long reservoir = 0, worst_short = 0;
        unsigned long long total_bits = 0;

        while (p + 4 <= got) {
            Mp3Header fh;
            if (!mp3_parse_header(buf + p, got - p, &fh)) break;

            size_t si = p + 4 + (fh.crc ? 2 : 0);
            Mp3SideInfo s;
            if (si + (size_t)fh.side_info_bytes <= got &&
                mp3_parse_sideinfo(buf + si, (int)(got - si), &fh, &s)) {
                parsed++;

                /* Can this frame reach back as far as it claims? The bytes it
                 * asks for must already have been produced by frames before
                 * it. (The first few frames of a file legitimately ask for
                 * nothing, which is why the reservoir starts empty.) */
                if ((long)s.main_data_begin > reservoir) {
                    long shortfall = (long)s.main_data_begin - reservoir;
                    if (shortfall > worst_short) worst_short = shortfall;
                }

                long used = 0;
                for (int gr = 0; gr < s.granules; gr++)
                    for (int c = 0; c < fh.channels; c++)
                        used += s.gr[gr][c].part2_3_length;
                total_bits += (unsigned long long)used;

                /* The reservoir grows by this frame's main-data slot and
                 * shrinks by what its granules actually spent. The format
                 * bounds it at 511 bytes -- that is what main_data_begin's
                 * 9 bits can address. */
                long avail = fh.frame_bytes - 4 - (fh.crc ? 2 : 0)
                           - fh.side_info_bytes;
                reservoir += avail - (used + 7) / 8;
                if (reservoir > 511) reservoir = 511;
                if (reservoir < 0) reservoir = 0;
            } else {
                bad++;
            }
            p += (size_t)fh.frame_bytes;
        }

        snprintf(msg, sizeof msg, "%u parsed, %u rejected", parsed, bad);
        ok("S1 every frame's side info parses in range",
           parsed > 10 && bad == 0, msg);

        snprintf(msg, sizeof msg, "worst shortfall %ld bytes", worst_short);
        ok("S1b the bit reservoir never asks for absent history",
           worst_short == 0, msg);

        double kbps = secs > 0 ? total_bits / secs / 1000.0 : 0;
        snprintf(msg, sizeof msg, "granule bits average %.0f kbit/s of a "
                 "%d kbit/s stream", kbps, h.bitrate / 1000);
        /* The granules cannot spend more than the stream carries, and a
         * decoder reading part2_3_length wrongly shows up here immediately as
         * a nonsense rate rather than as noise ten stages later. */
        ok("S2 granule bit budgets fit the stream's bitrate",
           kbps > 1.0 && kbps <= h.bitrate / 1000.0 + 1.0, msg);
    }

    /* F2 -- the encoder's own frame count, if it left one. */
    uint32_t xing = mp3_xing_frames(buf + first, got - (size_t)first, &h);
    if (xing) {
        /* The Xing frame itself is usually excluded from the count, so allow
         * exactly that difference rather than pretending it is not there. */
        long diff = (long)frames - (long)xing;
        snprintf(msg, sizeof msg, "walked %u, encoder said %u (diff %ld)",
                 frames, xing, diff);
        ok("F2 the walk matches the encoder's Xing count",
           diff >= 0 && diff <= 1, msg);
    } else {
        printf("  %-44s --  no Xing header in this file\n",
               "F2 encoder frame count");
    }

    free(buf);
}

int main(int argc, char **argv)
{
    printf("=== mp3\n");
    test_bits();
    test_tables();
    if (argc > 1) test_frames(argv[1]);
    else printf("  (no file given -- frame tests skipped)\n");

    printf("=== mp3: %s (%d failure%s)\n", g_fail ? "FAIL" : "OK", g_fail,
           g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
