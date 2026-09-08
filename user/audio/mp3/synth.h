/* user/audio/mp3/synth.h -- 32 subbands back into one waveform.
 *
 * The outer filterbank, and the last thing between the file and the speaker.
 * The encoder split the signal into 32 equal bands with a polyphase filter;
 * this reassembles them, and it is not a sum -- the analysis filters overlap,
 * so reconstruction needs a 512-point window applied across a 1024-sample
 * HISTORY per channel. Each output sample depends on the last half-second's
 * worth of subband values, not just the current ones.
 *
 * That history is the third piece of cross-frame state (with the reservoir and
 * the IMDCT overlap), and it is why the very first samples a decoder emits are
 * not quite right and why nobody notices: the window ramps in.
 */
#ifndef _EMBLINK_MP3_SYNTH_H_
#define _EMBLINK_MP3_SYNTH_H_

/* One granule, one channel: 32 subbands x 18 -> 576 PCM samples.
 *
 * `fifo` is the caller's per-channel history (1024 floats). Output is FLOAT
 * and unclamped; converting to 16-bit is the caller's business, because
 * clipping policy belongs where the format is known. */
void mp3_synth_granule(const float sb[32][18], float fifo[1024], float out[576]);

#endif /* _EMBLINK_MP3_SYNTH_H_ */
