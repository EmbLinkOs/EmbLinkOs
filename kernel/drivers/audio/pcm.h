/* kernel/drivers/audio/pcm.h -- what a sound card has to be able to do.
 *
 * WHY THIS FILE EXISTS. There were two audio drivers before it, ac97.c and
 * virtio_snd.c, and they were "interchangeable" in the way two files that
 * define the same symbols are interchangeable: exactly one of them could be
 * LINKED, so the choice was made in the Makefile, per architecture, at build
 * time. That is not a driver model. It works only as long as no machine has
 * two sound cards and no kernel image has to run on more than one machine --
 * and the whole point of this tree is to boot on a machine nobody has
 * described in advance.
 *
 * So the seam is a table now, the same shape as `struct net_driver`: every
 * driver is compiled in, each one probes, and the first that finds its
 * hardware wins. Adding Intel HDA is what forced it -- HDA and AC'97 are both
 * Intel, both PCI, both x86, and a machine can have either.
 *
 * THE CONTRACT IS AC'97-SHAPED and it is worth saying why rather than
 * pretending otherwise: a ring of descriptors, an index saying which one is
 * playing, and a "last valid" mark the writer keeps pushing ahead. HDA's DMA
 * engine is cyclic and has no such mark, so hda.c reconstructs one. That is
 * the driver's job -- it is the one that knows its hardware -- and it is a
 * better trade than making every caller learn two models.
 */
#ifndef _EMBK_PCM_H_
#define _EMBK_PCM_H_

#include <stdint.h>
#include "include/types.h"

/* Every driver here uses 32 descriptors, because audio.c schedules against
 * that number and a device with fewer would need a second ring behind it. */
#define PCM_DESCRIPTORS 32

struct pcm_driver {
    const char *name;

    /* Find the hardware and make it ready to be filled. False means "not this
     * machine" and is not a failure -- the next driver gets a turn. */
    bool     (*init)(void);

    uint32_t (*sample_rate)(void);

    /* The largest number of stereo frames one descriptor can hold. */
    uint32_t (*frames_per_buffer)(void);

    /* Copy `nframes` interleaved stereo S16 into descriptor `i`, returning how
     * many were TAKEN -- the caller keeps the rest for the next descriptor. */
    uint32_t (*fill)(int i, const int16_t *frames, uint32_t nframes);

    /* HOW LONG DESCRIPTOR `i` WILL ACTUALLY PLAY, in frames, which is not
     * always what fill() took. AC'97 writes the true length into the
     * descriptor, so the two agree. HDA's engine walks a buffer of fixed total
     * size and cannot be told a short one mid-stream, so a partial fill is
     * padded with silence and the device plays the padding too.
     *
     * The distinction is the difference between a correct playback position
     * and one that drifts by the padding on every short write -- and the
     * position is what A/V sync is built on. May be NULL, meaning "the same as
     * what fill() took". */
    uint32_t (*desc_frames)(int i);

    /* Which descriptor is playing right now, 0..PCM_DESCRIPTORS-1. */
    uint8_t  (*civ)(void);

    /* Extend the valid range to `last` without restarting. This is what makes
     * a stream rather than a one-shot. */
    void     (*set_last)(int last);

    /* Start, with descriptors 0..last filled. */
    void     (*play)(int last);

    /* Has everything up to `last` come out of the speaker? */
    bool     (*done)(int last);

    void     (*stop)(void);

    /* How many times the device ran out of audio mid-sound. The hardware's own
     * account where it keeps one, the driver's where it does not -- never an
     * inference from timings above. */
    uint64_t (*underruns)(void);
};

/* Each driver's table, defined at the bottom of its own file. */
extern const struct pcm_driver ac97_driver;        /* x86: Intel 82801AA    */
extern const struct pcm_driver hda_driver;         /* x86: Intel HD Audio   */
extern const struct pcm_driver virtio_snd_driver;  /* aarch64: virtio-sound */

#endif
