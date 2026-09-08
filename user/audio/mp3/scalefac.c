/* user/audio/mp3/scalefac.c -- see scalefac.h. */
#include <string.h>

#include "scalefac.h"

/* How many bits each scalefactor takes, from scalefac_compress. Two widths:
 * slen1 for the low bands, slen2 for the high ones, because the low bands need
 * more range and the high ones are usually near zero. */
static const uint8_t k_slen1[16] = { 0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4 };
static const uint8_t k_slen2[16] = { 0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3 };

/* The four band groups SCFSI can switch on, as [first, last] inclusive. */
static const uint8_t k_scfsi_band[4][2] = { { 0, 5 }, { 6, 10 }, { 11, 15 }, { 16, 20 } };

int mp3_read_scalefactors(BitReader *b, const Mp3SideInfo *si, int gr, int ch,
                          bool mpeg1, Mp3Scalefac *sf, const Mp3Scalefac *prev)
{
    if (!mpeg1) return -1;

    const Mp3Granule *g = &si->gr[gr][ch];
    int slen1 = k_slen1[g->scalefac_compress & 15];
    int slen2 = k_slen2[g->scalefac_compress & 15];
    size_t start = bits_pos(b);

    memset(sf, 0, sizeof *sf);

    if (g->block_type == BLOCK_SHORT) {
        /* SCFSI does not apply to short blocks -- a granule that switched its
         * window did so because the signal changed abruptly, which is exactly
         * when the previous granule's scalefactors are worthless. */
        int sfb = 0;
        if (g->mixed_block) {
            /* The lowest bands keep a long window (the transient is up high),
             * so they carry long scalefactors and the short ones start at 3. */
            for (; sfb < 8; sfb++) sf->l[sfb] = (uint8_t)bits_read(b, slen1);
            for (sfb = 3; sfb < 6; sfb++)
                for (int w = 0; w < 3; w++) sf->s[sfb][w] = (uint8_t)bits_read(b, slen1);
        } else {
            for (; sfb < 6; sfb++)
                for (int w = 0; w < 3; w++) sf->s[sfb][w] = (uint8_t)bits_read(b, slen1);
        }
        for (sfb = 6; sfb < 12; sfb++)
            for (int w = 0; w < 3; w++) sf->s[sfb][w] = (uint8_t)bits_read(b, slen2);
        /* Band 12 is never transmitted; it exists so the band table has an
         * upper bound to stop at. */
    } else {
        for (int grp = 0; grp < 4; grp++) {
            int lo = k_scfsi_band[grp][0], hi = k_scfsi_band[grp][1];
            int width = (grp < 2) ? slen1 : slen2;

            /* Granule 1 may inherit this group from granule 0. Only granule 1,
             * and only when the bit is set -- reading it for granule 0 would
             * consume bits that are not there. */
            /* BIT ORDER. scfsi is read as one 4-bit field, MSB first, so the
             * FIRST bit read -- value bit 3 -- is band group 0. Testing
             * (1 << grp) walks the groups backwards, which inherits the wrong
             * bands: granule 1 then skips reads it should have made (or makes
             * reads it should have skipped) and ends in the wrong place.
             *
             * It hid because most encoders never set scfsi at all. A whole
             * 8490-frame song passed while two 2-second test files did not,
             * which is the argument for testing several encoders rather than
             * one big file. */
            if (gr == 1 && prev && (si->scfsi[ch] & (1 << (3 - grp)))) {
                for (int sfb = lo; sfb <= hi; sfb++) sf->l[sfb] = prev->l[sfb];
                continue;
            }
            for (int sfb = lo; sfb <= hi; sfb++)
                sf->l[sfb] = (uint8_t)bits_read(b, width);
        }
    }

    return (int)(bits_pos(b) - start);
}
