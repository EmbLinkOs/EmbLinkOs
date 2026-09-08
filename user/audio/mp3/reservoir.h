/* user/audio/mp3/reservoir.h -- the bytes a granule is actually made of.
 *
 * A frame's audio data is not in that frame. main_data_begin says it starts
 * that many bytes BEFORE the end of what has been received so far, so a
 * granule that needed more bits than its slot allows spends the unused tails
 * of frames already decoded. The format calls it a bit reservoir; in practice
 * it means the decoder cannot point a bit reader at a frame and start reading.
 *
 * It has to keep a rolling window of recent main data instead, and 511 bytes
 * is the most any frame can reach back -- that is what a 9-bit field can say.
 *
 * A consequence worth stating because it looks like a bug when it appears:
 * after seeking, the first frames decode WRONG, because their data lives in
 * frames that were skipped. Nothing is broken; the reservoir simply has not
 * refilled. Players hide it by decoding a few frames before unmuting.
 */
#ifndef _EMBLINK_MP3_RESERVOIR_H_
#define _EMBLINK_MP3_RESERVOIR_H_

#include <stdint.h>
#include <stddef.h>

/* Enough for the 511 bytes of reach plus the largest frame's own main data,
 * with room to spare so compaction is rare rather than per frame. */
#define MP3_RES_CAP 4096

typedef struct {
    uint8_t buf[MP3_RES_CAP];
    size_t  len;
} Mp3Reservoir;

void mp3_res_reset(Mp3Reservoir *r);

/* Add one frame's main data and say where its granules begin.
 *
 * Returns the BIT position within `r->buf` at which this frame's data starts,
 * or -1 when main_data_begin reaches further back than we hold -- which is not
 * corruption at the start of a file or after a seek, it is the reservoir being
 * cold, and the caller should output silence for that frame rather than decode
 * whatever happens to be in the buffer. */
long mp3_res_add(Mp3Reservoir *r, const uint8_t *data, int len,
                 int main_data_begin);

#endif /* _EMBLINK_MP3_RESERVOIR_H_ */
