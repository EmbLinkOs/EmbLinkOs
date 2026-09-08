/* user/audio/mp3/spectrum.h -- integers back into a spectrum.
 *
 * The Huffman decoder produces small integers. They are not the signal: they
 * are what is left after the encoder divided the signal by however much noise
 * it decided each band could hide. Undoing that is three steps, and they must
 * happen in this order because each assumes the last.
 *
 *   REQUANTISE  raise to the 4/3 power and scale by the band's gain. The 4/3
 *               is not decoration -- the encoder quantised with a 3/4 power
 *               law, which spends more precision on small values, where the
 *               ear is more sensitive to error.
 *   REORDER     short blocks arrive grouped by WINDOW, because that is how
 *               they were coded; the IMDCT needs them grouped by FREQUENCY.
 *   STEREO      undo mid/side and intensity coding, which store the two
 *               channels as sum/difference or as one channel plus a position.
 */
#ifndef _EMBLINK_MP3_SPECTRUM_H_
#define _EMBLINK_MP3_SPECTRUM_H_

#include <stdint.h>

#include "sideinfo.h"
#include "scalefac.h"

/* Quantised integers -> real coefficients, for one granule of one channel. */
void mp3_requantise(const int32_t is[576], const Mp3Granule *g,
                    const Mp3Scalefac *sf, int sr_index, float xr[576]);

/* Short blocks: window-major -> frequency-major. No-op for long blocks. */
void mp3_reorder(float xr[576], const Mp3Granule *g, int sr_index);

/* Undo joint stereo across the pair. `mode_ext` says which of MS and intensity
 * is in use; both, either or neither is legal. */
void mp3_stereo(float left[576], float right[576], const Mp3Granule *g,
                const Mp3Scalefac *sf_right, int mode, int mode_ext,
                int sr_index);

#endif /* _EMBLINK_MP3_SPECTRUM_H_ */
