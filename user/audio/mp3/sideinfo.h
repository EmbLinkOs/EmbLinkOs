/* user/audio/mp3/sideinfo.h -- the frame's instructions for decoding itself.
 *
 * After the header comes a fixed-size block that describes HOW the audio data
 * that follows is coded: how many bits each granule spent, which Huffman
 * tables it used, whether it is a long block or three short ones, and -- the
 * part that surprises everyone -- WHERE ITS DATA ACTUALLY IS.
 *
 * THE BIT RESERVOIR. A frame's audio data does not have to live inside that
 * frame. `main_data_begin` is a NEGATIVE offset: it says the data starts that
 * many bytes BEFORE the end of the previous frame's data, so a frame that
 * needed more bits than its fixed size allows borrows the unused tail of the
 * frames before it. This is why an MP3 cannot simply be decoded frame by frame
 * from a single buffer, why seeking mid-file produces a few bad frames until
 * the reservoir refills, and why the decoder keeps a rolling window of past
 * bytes rather than a pointer into the current frame.
 */
#ifndef _EMBLINK_MP3_SIDEINFO_H_
#define _EMBLINK_MP3_SIDEINFO_H_

#include <stdint.h>
#include <stdbool.h>

#include "frame.h"

/* Block types. A "short" granule splits its 18-sample window into three of six
 * to follow a transient -- which is what stops a cymbal from smearing
 * backwards into the silence before it (pre-echo). */
enum { BLOCK_LONG = 0, BLOCK_START = 1, BLOCK_SHORT = 2, BLOCK_STOP = 3 };

typedef struct {
    uint16_t part2_3_length;   /* bits of scalefactors + Huffman data     */
    uint16_t big_values;       /* pairs coded with the big-value tables   */
    uint8_t  global_gain;      /* the granule's overall level             */
    uint8_t  scalefac_compress;
    bool     window_switching;
    uint8_t  block_type;       /* BLOCK_*                                 */
    bool     mixed_block;      /* lowest bands long, the rest short       */
    uint8_t  table_select[3];
    uint8_t  subblock_gain[3];
    uint8_t  region0_count, region1_count;
    bool     preflag;
    bool     scalefac_scale;
    bool     count1table_select;
} Mp3Granule;

typedef struct {
    uint16_t   main_data_begin;      /* bytes BACK to where the data starts */
    uint8_t    private_bits;
    uint8_t    scfsi[2];             /* per channel: reuse last granule's
                                      * scalefactors for a band group       */
    Mp3Granule gr[2][2];             /* [granule][channel]                  */
    int        granules;             /* 2 on MPEG-1, 1 otherwise            */
} Mp3SideInfo;

/* Parse the side info that follows the header (and CRC, if present).
 * `data` points at the first side-info byte. Returns false if a field is out
 * of range -- which is worth checking rather than trusting, because these
 * values index tables and a bad one is an out-of-bounds read on input the
 * decoder did not write. */
bool mp3_parse_sideinfo(const uint8_t *data, int size,
                        const Mp3Header *h, Mp3SideInfo *out);

#endif /* _EMBLINK_MP3_SIDEINFO_H_ */
