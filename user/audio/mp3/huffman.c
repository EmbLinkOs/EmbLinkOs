/* user/audio/mp3/huffman.c -- see huffman.h. */
#include <string.h>

#include "huffman.h"
#include "tables.h"

/* Where each code length lives inside a table's span.
 *
 * tables.c stores every code sorted by (length, code), so the codes of a given
 * length are CONTIGUOUS and in increasing order. That is the whole search
 * structure: read a bit, extend the accumulator, and binary-search the run for
 * that length. No tree to build, no decode table to allocate, and the memory
 * is exactly the codes themselves.
 *
 * Built once, lazily. 34 tables x 20 lengths of two uint16 is 2.7KB, against
 * the ~15KB a 9-bit-per-table lookup would take -- and on a decoder that
 * spends most of its time in the IMDCT and the filterbank, not here, that is
 * the right trade. */
static struct { uint16_t start, count; } g_span[34][20];
static bool g_indexed;

/* Where the last failure happened. Diagnostics only: a decoder that can only
 * say "this granule was bad" makes you guess which of six interacting things
 * was wrong, and guessing is what costs the days. */
int mp3_huff_err_table = -1, mp3_huff_err_index = -1, mp3_huff_err_phase = -1;

static void build_index(void)
{
    memset(g_span, 0, sizeof g_span);
    for (int t = 0; t < 34; t++) {
        const Mp3HuffTable *ht = &mp3_huff_tables[t];
        for (uint16_t i = 0; i < ht->count; i++) {
            uint8_t bits = mp3_huff_entries[ht->start + i].bits;
            if (bits == 0 || bits >= 20) continue;
            if (g_span[t][bits].count == 0) g_span[t][bits].start = i;
            g_span[t][bits].count++;
        }
    }
    g_indexed = true;
}

/* Read one code from table `t`. Returns the entry, or NULL if no code matched
 * within 19 bits -- which means the data is corrupt, not that we should guess. */
static const Mp3HuffEntry *read_code(BitReader *b, int t)
{
    const Mp3HuffTable *ht = &mp3_huff_tables[t];
    if (ht->count == 0) return NULL;

    uint32_t acc = 0;
    for (int len = 1; len < 20; len++) {
        acc = (acc << 1) | bits_read(b, 1);
        uint16_t n = g_span[t][len].count;
        if (n == 0) continue;

        const Mp3HuffEntry *base = &mp3_huff_entries[ht->start + g_span[t][len].start];
        uint16_t lo = 0, hi = n;                 /* sorted by code */
        while (lo < hi) {
            uint16_t mid = (uint16_t)((lo + hi) / 2);
            if (base[mid].code < acc) lo = (uint16_t)(mid + 1);
            else                      hi = mid;
        }
        if (lo < n && base[lo].code == acc) return &base[lo];
    }
    return NULL;
}

/* One (x, y) pair, with the escape and sign handling the standard specifies.
 *
 * A quantised value of 15 in a table with linbits does not mean 15: it means
 * "at least 15", and that many extra magnitude bits follow. Then a sign bit,
 * but ONLY for values that are non-zero -- coding a sign for zero would waste
 * a bit per zero coefficient, and zeros are most of them. */
static void read_pair(BitReader *b, const Mp3HuffEntry *e, int linbits,
                      int32_t *x, int32_t *y)
{
    int32_t vx = e->x, vy = e->y;

    if (linbits && vx == 15) vx += (int32_t)bits_read(b, linbits);
    if (vx && bits_read(b, 1)) vx = -vx;
    if (linbits && vy == 15) vy += (int32_t)bits_read(b, linbits);
    if (vy && bits_read(b, 1)) vy = -vy;

    *x = vx;
    *y = vy;
}

/* Region boundaries, in coefficients.
 *
 * For a normal granule the two counts index SCALEFACTOR BANDS, so the split
 * follows the psychoacoustic band edges rather than round numbers. When the
 * window is switching there are only two regions and the first ends at a fixed
 * 36 -- the counts in the side info are not read at all in that case, which is
 * why sideinfo.c synthesises them. */
static void regions(const Mp3Granule *g, int sr_index, int big_values,
                    int *r1, int *r2)
{
    int limit = big_values * 2;

    if (g->window_switching) {
        *r1 = 36;
        *r2 = 576;
    } else {
        const unsigned *sfb = mp3_sf_bands[sr_index].l;
        int i1 = g->region0_count + 1;
        int i2 = i1 + g->region1_count + 1;
        if (i1 > 22) i1 = 22;
        if (i2 > 22) i2 = 22;
        *r1 = (int)sfb[i1];
        *r2 = (int)sfb[i2];
    }
    if (*r1 > limit) *r1 = limit;
    if (*r2 > limit) *r2 = limit;
}

int mp3_huffman_granule(BitReader *b, size_t end_bit,
                        const Mp3Granule *g, int sr_index,
                        int32_t out[576])
{
    if (!g_indexed) build_index();
    memset(out, 0, 576 * sizeof out[0]);

    int big = g->big_values * 2;
    if (big > 576) big = 576;

    int r1 = 0, r2 = 0;
    regions(g, sr_index, g->big_values, &r1, &r2);

    int i = 0;
    for (; i < big; i += 2) {
        int table = (i < r1) ? g->table_select[0]
                  : (i < r2) ? g->table_select[1]
                             : g->table_select[2];
        /* TABLE 0 IS NOT A MISSING TABLE. Selecting it means the region is
         * entirely zeros and costs NO bits at all -- the cheapest way the
         * format has of saying "nothing up here". Treating it as a decode
         * failure is what made 413 granules of real music fail while a
         * synthetic sine passed perfectly: the sine never had a silent region
         * to skip, so it never selected table 0. */
        if (mp3_huff_tables[table].count == 0) {
            out[i] = out[i + 1] = 0;
            continue;
        }

        /* Running out of bits mid-pair is corruption, and the honest response
         * is to stop with what we have rather than to read past the granule
         * and desynchronise everything after it. */
        if (bits_pos(b) >= end_bit) break;

        const Mp3HuffEntry *e = read_code(b, table);
        if (!e) {
            mp3_huff_err_table = table; mp3_huff_err_index = i;
            mp3_huff_err_phase = 0;                    /* big_values */
            return -1;
        }
        read_pair(b, e, mp3_huff_tables[table].linbits, &out[i], &out[i + 1]);
    }

    /* COUNT1: four coefficients per code, until the granule's bits run out.
     * The loop is bounded by BITS, not by a count -- nothing in the stream
     * says how many quadruples there are, which is exactly why the caller has
     * to hand us an exact end position. */
    int t1 = g->count1table_select ? 33 : 32;
    while (i + 4 <= 576 && bits_pos(b) < end_bit) {
        const Mp3HuffEntry *e = read_code(b, t1);
        /* A code that does not decode while bits REMAIN is corruption, and
         * this used to `break` -- which quietly produced a short granule that
         * looked exactly like an encoder padding its bit budget. Two very
         * different things reaching the same silence is how a decoder bug
         * hides, so they are separated: this returns failure, and running out
         * of bits ends the loop normally via the condition above. */
        if (!e) {
            mp3_huff_err_table = t1; mp3_huff_err_index = i;
            mp3_huff_err_phase = 1;                    /* count1 */
            return -1;
        }

        /* The four values are packed one per bit in the symbol, each 0 or 1,
         * with a sign bit following every non-zero one. */
        int v = (e->y >> 3) & 1, w = (e->y >> 2) & 1;
        int x = (e->y >> 1) & 1, y = e->y & 1;
        int32_t q[4] = { v, w, x, y };
        for (int k = 0; k < 4; k++)
            if (q[k] && bits_read(b, 1)) q[k] = -q[k];

        out[i + 0] = q[0]; out[i + 1] = q[1];
        out[i + 2] = q[2]; out[i + 3] = q[3];
        i += 4;
    }

    /* Overrunning the granule is NORMAL for count1 and the standard says so:
     * the encoder writes whole quadruples, and the last one can cross the
     * boundary. The caller repositions to end_bit regardless, so the overrun
     * costs those four coefficients and nothing else. */
    return i;
}
