/* user/audio/mp3/sideinfo.c -- see sideinfo.h. */
#include <string.h>

#include "bits.h"
#include "sideinfo.h"

/* MPEG-1 and MPEG-2 lay this out differently -- not just in size but in which
 * fields exist. MPEG-2 dropped to one granule and replaced scfsi with a
 * per-channel scalefactor-compression scheme, so the two are separate readers
 * rather than one reader with flags sprinkled through it. */
static bool parse_granule(BitReader *b, Mp3Granule *g, bool mpeg1)
{
    g->part2_3_length    = (uint16_t)bits_read(b, 12);
    g->big_values        = (uint16_t)bits_read(b, 9);
    g->global_gain       = (uint8_t)bits_read(b, 8);
    g->scalefac_compress = (uint8_t)bits_read(b, mpeg1 ? 4 : 9);
    g->window_switching  = bits_read(b, 1) != 0;

    /* big_values counts PAIRS and addresses 2*big_values coefficients out of
     * 576. A frame claiming more than 288 is malformed, and since this value
     * drives the Huffman loop's bounds, believing it is an overrun. */
    if (g->big_values > 288) return false;

    if (g->window_switching) {
        g->block_type  = (uint8_t)bits_read(b, 2);
        g->mixed_block = bits_read(b, 1) != 0;
        for (int i = 0; i < 2; i++) g->table_select[i] = (uint8_t)bits_read(b, 5);
        g->table_select[2] = 0;
        for (int i = 0; i < 3; i++) g->subblock_gain[i] = (uint8_t)bits_read(b, 3);

        /* Only two regions exist when window switching, so the counts are
         * FIXED rather than read -- and the values are not 0: region0 is 8
         * (or 9 for short blocks without mixing) scalefactor bands. Hardcoding
         * the wrong constant here is a bug that only shows up on transients. */
        g->region0_count = (g->block_type == BLOCK_SHORT && !g->mixed_block) ? 8 : 7;
        g->region1_count = 20 - g->region0_count;

        /* block_type 0 with window_switching set is forbidden: the flag says a
         * non-normal window follows, and 0 means the normal one. */
        if (g->block_type == BLOCK_LONG) return false;
    } else {
        g->block_type  = BLOCK_LONG;
        g->mixed_block = false;
        for (int i = 0; i < 3; i++) g->table_select[i] = (uint8_t)bits_read(b, 5);
        memset(g->subblock_gain, 0, sizeof g->subblock_gain);
        g->region0_count = (uint8_t)bits_read(b, 4);
        g->region1_count = (uint8_t)bits_read(b, 3);
    }

    g->preflag           = mpeg1 ? bits_read(b, 1) != 0 : false;
    g->scalefac_scale    = bits_read(b, 1) != 0;
    g->count1table_select = bits_read(b, 1) != 0;

    /* There are 32 Huffman tables and several indices are unused; an index
     * past the end would be a table lookup out of bounds. */
    for (int i = 0; i < 3; i++) if (g->table_select[i] > 31) return false;
    return true;
}

bool mp3_parse_sideinfo(const uint8_t *data, int size,
                        const Mp3Header *h, Mp3SideInfo *out)
{
    if (size < h->side_info_bytes) return false;

    BitReader b;
    bits_init(&b, data, (size_t)h->side_info_bytes);
    memset(out, 0, sizeof *out);

    bool mpeg1 = (h->version == MPEG_1);
    int  ch    = h->channels;
    out->granules = mpeg1 ? 2 : 1;

    /* main_data_begin is 9 bits on MPEG-1 and 8 on MPEG-2 -- the reservoir is
     * smaller there. Reading the wrong width shifts every field after it, and
     * because the rest still parses into plausible-looking numbers the symptom
     * is noise rather than an error. */
    out->main_data_begin = (uint16_t)bits_read(&b, mpeg1 ? 9 : 8);

    if (mpeg1) {
        bits_skip(&b, ch == 1 ? 5 : 3);                  /* private_bits */
        for (int c = 0; c < ch; c++) out->scfsi[c] = (uint8_t)bits_read(&b, 4);
    } else {
        bits_skip(&b, 1);                                /* private_bits */
    }

    for (int gr = 0; gr < out->granules; gr++)
        for (int c = 0; c < ch; c++)
            if (!parse_granule(&b, &out->gr[gr][c], mpeg1)) return false;

    /* If the reader ran off the end, the layout assumed here does not match
     * the file, and every value read is suspect. */
    return !b.overrun;
}
