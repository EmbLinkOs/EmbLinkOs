/* user/audio/mp3/scalefac.h -- how loud each band is allowed to be wrong.
 *
 * Quantisation noise is only inaudible if it stays under the music that masks
 * it, and how much room there is differs per frequency band and per moment. So
 * the encoder sends a scalefactor per band: the amount the decoder must
 * multiply that band back up by. They are the psychoacoustic model's output,
 * written down.
 *
 * SCFSI is the compression trick that makes them cheap. Scalefactors change
 * slowly, so a frame's second granule can say "reuse the first granule's
 * values for this group of bands" instead of resending them -- which means the
 * decoder cannot read granule 1 without having kept granule 0, and a decoder
 * that seeks into the middle of a file gets this wrong until the next frame
 * that resends everything.
 */
#ifndef _EMBLINK_MP3_SCALEFAC_H_
#define _EMBLINK_MP3_SCALEFAC_H_

#include <stdint.h>
#include <stdbool.h>

#include "bits.h"
#include "sideinfo.h"

typedef struct {
    uint8_t l[23];        /* long blocks: one per scalefactor band     */
    uint8_t s[13][3];     /* short blocks: per band, per of 3 windows  */
} Mp3Scalefac;

/* Read granule `gr` channel `ch`'s scalefactors. `prev` is the same channel's
 * granule 0 values, which SCFSI may say to reuse (pass NULL for granule 0).
 * Returns the number of bits consumed, or -1 if unsupported.
 *
 * MPEG-1 only for now. MPEG-2 and 2.5 replaced this whole scheme with a
 * different partitioning, and half-implementing it would produce audio rather
 * than an error -- so it says so instead. */
int mp3_read_scalefactors(BitReader *b, const Mp3SideInfo *si, int gr, int ch,
                          bool mpeg1, Mp3Scalefac *sf, const Mp3Scalefac *prev);

#endif /* _EMBLINK_MP3_SCALEFAC_H_ */
