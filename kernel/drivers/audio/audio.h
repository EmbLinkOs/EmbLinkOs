/* kernel/drivers/audio/audio.h -- the PCM sink the syscalls sit on.
 *
 * Device-independent on purpose: everything here is about a stream and its
 * owner, and nothing about ports or descriptors. A second driver implements
 * ac97.h; this file does not change.
 *
 * Format is fixed at the device's own: 16-bit signed, stereo, interleaved, at
 * audio_sample_rate(). Resampling and mixing are a level above a sink that
 * has one owner, and doing them badly in the kernel is worse than not doing
 * them -- see audio.c on why there is no mixer yet.
 */
#ifndef _EMBK_AUDIO_H_
#define _EMBK_AUDIO_H_

#include <stdint.h>
#include "include/types.h"

bool     audio_available(void);
uint32_t audio_sample_rate(void);
uint32_t audio_frames_per_buffer(void);

/* Claim the device. -EMBK_EBUSY if another process holds it, -EMBK_ENODEV if
 * there is no hardware. Re-opening by the same pid is not an error. */
int  audio_open(uint32_t pid);

/* Queue interleaved stereo S16. Writes what FITS and reports it in *accepted:
 * a short write means the ring is full, not that anything failed. */
int  audio_write(uint32_t pid, const int16_t *frames, uint32_t nframes,
                 uint32_t *accepted);

/* Has the hardware played everything handed to it? */
bool audio_drained(uint32_t pid);

/* WHERE THE SPEAKER ACTUALLY IS, in frames since this stream started;
 * monotonic, 0 before the device begins. The device itself reports only which
 * descriptor it is on -- a number that wraps and answers nothing alone -- so
 * the laps are counted for you.
 *
 * This is what a writer needs to hold a KNOWN distance ahead, and what A/V
 * sync needs to line sound up with picture. Do not substitute the wall clock:
 * the device does not start at t=0, it starts when the prefill is met, so a
 * clock-based estimate is ahead of the truth by exactly the prefill.
 *
 * Resolution is one descriptor (~21 ms at 48 kHz) -- it is the position of the
 * buffer being played, not of the sample. */
uint64_t audio_position(uint32_t pid);

/* Ask for a shallower start-up buffer: the device will begin once `ms` of
 * audio is queued instead of the default ~170 ms. THAT NUMBER IS THE LATENCY
 * of everything the stream does afterwards, so a writer that can be relied on
 * to come back in time should lower it, and one that cannot should not --
 * running dry is worse than being late, and a caller who lowers this without
 * being able to keep up has chosen holes over delay.
 *
 * Only before the first sound comes out; -EBUSY afterwards. Returns the
 * latency actually granted in ms, which is rounded UP to whole descriptors and
 * floored at two of them. */
int audio_set_latency(uint32_t pid, uint32_t ms);

void audio_close(uint32_t pid);
void audio_reap_pid(uint32_t pid);   /* a process died holding the device */

/* WHAT THE STREAM ACTUALLY DID. `underruns` is how many times the speaker ran
 * dry mid-sound -- the hardware's own count, not an inference -- and it is the
 * only audio failure a person hears directly. `frames` is how much was handed
 * over, `writes` how many calls it took. All three are since the current sound
 * started. Any may be NULL. */
void audio_stats(uint64_t *underruns, uint64_t *frames, uint64_t *writes);

#endif
