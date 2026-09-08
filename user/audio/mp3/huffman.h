/* user/audio/mp3/huffman.h -- turning bits back into 576 coefficients.
 *
 * A granule's spectrum is coded in three stretches, and they are not the same
 * kind of thing:
 *
 *   BIG VALUES  pairs of coefficients, each pair one Huffman code, split into
 *               three REGIONS that may each use a different table. The regions
 *               exist because low frequencies and high frequencies have very
 *               different statistics, and one table for all of them would cost
 *               bits everywhere to be adequate anywhere.
 *   COUNT1      the tail, where nearly everything has quantised to 0 or +-1.
 *               Four coefficients per code, and only two tables in the whole
 *               standard -- one Huffman, one a flat 4-bit code.
 *   ZEROS       the rest of the 576, never coded at all. Silence above some
 *               frequency is the single biggest thing the format saves on.
 *
 * The decoder does NOT know how many coefficients it will get. It knows how
 * many BITS the granule spent (part2_3_length, from the side info), and it
 * reads count1 quadruples until those bits run out. That is why bit-exact
 * accounting is not a nicety here: overrun by one bit and the next granule
 * starts in the wrong place.
 */
#ifndef _EMBLINK_MP3_HUFFMAN_H_
#define _EMBLINK_MP3_HUFFMAN_H_

#include <stdint.h>

#include "bits.h"
#include "sideinfo.h"

/* Decode one granule's spectrum into `out` (576 coefficients, signed).
 *
 * `end_bit` is the absolute bit position where this granule's data ends -- the
 * caller computes it from part2_3_length, because only the caller knows where
 * the granule started in the reservoir. Reading stops there even if the count1
 * loop would happily continue: a corrupt frame that claims more data than it
 * has must not walk into the next granule's bits.
 *
 * Returns the number of coefficients actually decoded (the rest of `out` is
 * zeroed), or -1 if the stream was malformed. */
extern int mp3_huff_err_table, mp3_huff_err_index, mp3_huff_err_phase;

int mp3_huffman_granule(BitReader *b, size_t end_bit,
                        const Mp3Granule *g, int samplerate_index,
                        int32_t out[576]);

#endif /* _EMBLINK_MP3_HUFFMAN_H_ */
