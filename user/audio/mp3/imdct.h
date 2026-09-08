/* user/audio/mp3/imdct.h -- the spectrum becomes 32 subbands of samples.
 *
 * Layer III is a filterbank inside a filterbank: 32 subbands from the
 * polyphase stage, each further split by an 18-point MDCT. This undoes the
 * inner one, and two things have to happen around it.
 *
 * ALIAS REDUCTION comes first. The 32-band polyphase filters overlap, so each
 * band contains a mirrored ghost of its neighbours. The encoder cancelled that
 * with butterflies across every band boundary; the decoder must apply the
 * inverse BEFORE transforming, or the ghosts stay. Short blocks skip it --
 * their windows are too narrow for the cancellation to be worth its cost.
 *
 * FREQUENCY INVERSION comes last. The polyphase synthesis expects every other
 * subband spectrally flipped, so odd samples of odd subbands are negated. It
 * is one sign flip and omitting it produces audio that is recognisably the
 * right music with a metallic edge -- the kind of bug you can listen past.
 */
#ifndef _EMBLINK_MP3_IMDCT_H_
#define _EMBLINK_MP3_IMDCT_H_

#include "sideinfo.h"

/* One granule, one channel: 576 coefficients -> 32 subbands x 18 samples.
 *
 * `overlap` is the caller's per-channel memory of the previous granule's tail.
 * The MDCT is 50% overlapped by construction, so a granule alone decodes to
 * nothing usable -- half of every output sample comes from the granule before
 * it. That is the state that makes seeking imperfect. */
void mp3_imdct_granule(const float xr[576], const Mp3Granule *g,
                       float overlap[32][18], float out[32][18]);

#endif /* _EMBLINK_MP3_IMDCT_H_ */
