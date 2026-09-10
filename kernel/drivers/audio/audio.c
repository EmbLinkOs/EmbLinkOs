/* kernel/drivers/audio/audio.c -- one PCM sink, and who is allowed to use it.
 *
 * The device-independent half. ac97.c knows about I/O ports and descriptor
 * lists; this knows about a stream: who owns it, how far the hardware has got,
 * and what to do when a program writes faster or slower than the speaker
 * consumes. A second driver (Intel HDA, virtio-sound) implements ac97.h's four
 * operations and nothing here changes.
 *
 * ONE OWNER AT A TIME, deliberately. Mixing several streams means resampling,
 * summing and clipping policy, and a mixer that is wrong is worse than no
 * mixer -- it makes every program quieter and none of them correct. A second
 * opener is REFUSED with a real error rather than silently sharing, so the
 * failure is visible at the call that caused it. The owner is a pid, so a
 * process that dies holding the device does not keep it forever.
 *
 * THE RING is the driver's 32 descriptors used as a circle. `head` is the next
 * descriptor to fill; the hardware's CIV says which it is playing. Writing is
 * allowed while there is a gap between them -- and the gap is what makes this
 * a stream rather than a one-shot, because the writer keeps moving LVI ahead
 * of a device that never stops.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "drivers/audio/ac97.h"
#include "drivers/audio/audio.h"

#define RING          32          /* must match the driver's descriptor count */
/* Never fill the descriptor being played, or the one after it: the device may
 * already have prefetched the next, and rewriting it is a click the writer
 * cannot hear and cannot explain. */
#define RING_HEADROOM 2

/* PREFILL BEFORE STARTING. Starting the hardware on the first buffer means it
 * reaches the end of that buffer, halts at LVI, and waits for the writer --
 * once per buffer, for the whole sound. That is not a click, it is a stutter
 * with silence between every 21ms of audio, and under TCG (where the guest is
 * slower than real time) it is most of the output: `beep` measured 46%
 * non-silent and the checker read the gaps as HALF the frequency.
 *
 * So the device does not start until there is runway behind it. Eight buffers
 * is ~170ms at 48kHz, which is enough for the writer to stay ahead, and it is
 * the difference between a stream and a series of one-shots.
 *
 * IT IS ALSO 170 ms OF LATENCY, and that is the other half of the truth. Every
 * queued buffer sits between a program deciding to make a sound and the sound
 * existing. 170 ms is fine for a song and useless for a synth, a game, or
 * anything syncing to picture -- and a writer that KNOWS it will come back in
 * time should be allowed to say so. audio_set_latency() lowers this floor for
 * one stream; this stays the default, because a writer that has not thought
 * about it should get the value that works without thinking. */
#define RING_START_MIN 8

/* THE SHALLOWEST BUFFER THAT CAN BE PROMISED. Not a hardware limit -- the
 * descriptor list will happily hold a millisecond of audio -- but a limit on
 * what any scheduler can keep: a writer holding less than this has to be woken
 * and complete a refill inside one scheduling round on a machine with other
 * work, and granting it would be promising something the OS cannot deliver.
 * 10 ms is one timer tick, which is the honest smallest unit of "soon". */
#define AUDIO_MIN_LATENCY_MS 10u

static struct {
    bool     open;
    uint32_t owner_pid;
    int      head;                /* next descriptor to fill                  */
    int      last;                /* highest index handed to the hardware     */
    int      queued;              /* descriptors filled but not yet started   */
    bool     started;
    uint64_t frames_written;
    uint64_t writes;
    uint32_t start_frames;        /* frames to queue before starting          */
    uint32_t queued_frames;       /* frames filled but not yet started        */

    /* THE PLAYBACK POSITION, monotonic, in frames.
     *
     * The device reports only CIV -- which descriptor it is on, 0..RING-1, a
     * number that wraps and therefore answers nothing on its own. A writer
     * that wants to stay a known distance ahead needs to know where the
     * speaker actually IS, and inferring it from the wall clock is wrong in
     * the one case that matters: the device does not begin at t=0, it begins
     * when the prefill is met, so a clock-based estimate is ahead of the truth
     * by exactly the prefill and a writer using it holds far more runway than
     * it asked for. (That is not hypothetical -- it is what made the first
     * version of `test audiostress` measure nothing.)
     *
     * So the wraps are counted here, on every call that looks at CIV. This is
     * also the number A/V sync needs, which is why it is a real accessor and
     * not a test fixture. */
    int      last_civ;
    uint64_t played;              /* frames the device has finished           */
    uint32_t desc_frames[RING];   /* how long each descriptor actually is     */
} g_au;

/* Advance the monotonic position to wherever CIV has got to.
 *
 * Counting descriptors and multiplying by a fixed size stopped being correct
 * when a descriptor became as long as whatever was put in it -- so the length
 * of each one is remembered as it is filled, and this walks the ones the
 * device has finished since the last look. Walking rather than subtracting is
 * also what makes a wrap ordinary instead of a special case.
 *
 * Must be called by anything that reads CIV, and often enough that a full lap
 * of the ring cannot pass unseen between two calls -- at the shallow buffers
 * this now allows, that is a writer that has stopped writing, which is a
 * different failure and shows up as an underrun. */
static void position_poll(void)
{
    if (!g_au.started) return;
    int civ = (int)ac97_civ();
    if (g_au.last_civ < 0) { g_au.last_civ = civ; return; }
    /* Bounded by RING: an unbounded walk on a garbage CIV would hang here, and
     * a device that reported one has failed in a way this cannot fix. */
    for (int guard = 0; guard < RING && g_au.last_civ != civ; guard++) {
        g_au.played += g_au.desc_frames[g_au.last_civ];
        g_au.last_civ = (g_au.last_civ + 1) % RING;
    }
}

bool audio_available(void)
{
    return ac97_present();
}

uint32_t audio_sample_rate(void)
{
    return ac97_sample_rate();
}

uint32_t audio_frames_per_buffer(void)
{
    return ac97_frames_per_buffer();
}

int audio_open(uint32_t pid)
{
    if (!ac97_present()) return -EMBK_ENODEV;
    /* Re-opening by the SAME process is not an error -- a program that
     * restarts its own playback should not have to know whether it closed. */
    if (g_au.open && g_au.owner_pid != pid) return -EMBK_EBUSY;

    g_au.open = true;
    g_au.owner_pid = pid;
    g_au.head = 0;
    g_au.last = -1;
    g_au.queued = 0;
    g_au.started = false;
    g_au.frames_written = 0;
    g_au.writes = 0;
    g_au.start_frames = 0;        /* filled in below, once the rate is known */
    g_au.queued_frames = 0;
    g_au.last_civ = -1;
    g_au.played = 0;
    for (int i = 0; i < RING; i++) g_au.desc_frames[i] = 0;

    /* The default prefill, expressed in frames rather than descriptors now
     * that a descriptor is not a fixed size. Same duration as before. */
    g_au.start_frames = (uint32_t)RING_START_MIN * ac97_frames_per_buffer();
    return EMBK_OK;
}

int audio_set_latency(uint32_t pid, uint32_t ms)
{
    if (!ac97_present()) return -EMBK_ENODEV;
    if (!g_au.open || g_au.owner_pid != pid) return -EMBK_EPERM;
    /* Only before the device is running. Lowering the prefill of a stream that
     * has already started means nothing (it started), and raising it means
     * nothing either -- so a call that arrives late is a caller who thinks
     * something happened that did not. */
    if (g_au.started) return -EMBK_EBUSY;

    uint32_t rate = ac97_sample_rate();
    if (!rate) return -EMBK_ENODEV;

    /* THE FLOOR IS TWO DESCRIPTORS' WORTH OF WHATEVER THE WRITER SENDS, and it
     * cannot be expressed in frames here because the writer has not sent
     * anything yet. So the floor enforced here is a duration -- below
     * AUDIO_MIN_LATENCY_MS the device would reach the end of the list inside a
     * single scheduling round on any machine, and granting it would be
     * promising something no scheduler can keep. */
    if (ms < AUDIO_MIN_LATENCY_MS) ms = AUDIO_MIN_LATENCY_MS;

    uint64_t frames = (uint64_t)rate * ms / 1000;
    uint32_t cap    = (uint32_t)(RING - RING_HEADROOM) * ac97_frames_per_buffer();
    if (frames > cap) frames = cap;
    g_au.start_frames = (uint32_t)frames;

    /* What it actually got, in ms -- the caller asked in a unit the hardware
     * does not have, and the difference is theirs to know. */
    return (int)((uint64_t)g_au.start_frames * 1000 / rate);
}

uint64_t audio_position(uint32_t pid)
{
    if (!ac97_present() || !g_au.open || g_au.owner_pid != pid) return 0;
    if (!g_au.started) return 0;
    position_poll();
    return g_au.played;
}

void audio_stats(uint64_t *underruns, uint64_t *frames, uint64_t *writes)
{
    if (underruns) *underruns = ac97_present() ? ac97_underruns() : 0;
    if (frames)    *frames    = g_au.frames_written;
    if (writes)    *writes    = g_au.writes;
}

/* How many descriptors are free to write into right now. */
static int ring_free(void)
{
    if (!g_au.started) return RING - RING_HEADROOM;

    int civ = (int)ac97_civ();
    int used = g_au.head - civ;
    if (used < 0) used += RING;
    int freed = RING - used - RING_HEADROOM;
    return freed > 0 ? freed : 0;
}

int audio_write(uint32_t pid, const int16_t *frames, uint32_t nframes,
                uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!ac97_present()) return -EMBK_ENODEV;
    if (!g_au.open || g_au.owner_pid != pid) return -EMBK_EPERM;
    if (frames == NULL) return -EMBK_EINVAL;

    position_poll();
    uint32_t taken = 0;
    int slots = ring_free();

    while (slots > 0 && taken < nframes) {
        uint32_t n = ac97_fill(g_au.head, frames + (size_t)taken * 2,
                               nframes - taken);
        if (n == 0) break;
        taken += n;
        /* How long this descriptor is, remembered for position_poll(): the
         * device will not tell us, and a descriptor is no longer a fixed
         * size. */
        g_au.desc_frames[g_au.head] = n;
        g_au.last = g_au.head;
        g_au.head = (g_au.head + 1) % RING;
        g_au.queued++;
        if (!g_au.started) g_au.queued_frames += n;
        slots--;
    }

    if (taken > 0) {
        g_au.frames_written += taken;
        g_au.writes++;
        /* Start on the FIRST write, extend on every one after. Starting from
         * audio_open would run the device over empty descriptors and put a
         * burst of silence at the head of every sound. */
        if (!g_au.started) {
            /* Hold until there is enough queued to keep the device fed. A
             * writer with less than this in total gets it started by
             * audio_drained(), which is what "I have finished writing" means
             * from out here. */
            /* Two conditions, and the second is not a nicety: `queued` is
             * capped by the ring, so a writer sending very short chunks can
             * fill every descriptor without ever reaching start_frames, and
             * would then wait forever for room that only playing can free. */
            if (g_au.queued_frames >= g_au.start_frames ||
                g_au.queued >= RING - RING_HEADROOM) {
                ac97_play(g_au.last);
                g_au.started = true;
                g_au.last_civ = -1;
            }
        } else {
            ac97_set_last(g_au.last);
        }
    }

    if (accepted) *accepted = taken;
    /* Short writes are NORMAL and are not an error: the ring is full and the
     * caller should come back. Saying EAGAIN for a partial accept would make
     * every well-behaved writer look like it failed. */
    return EMBK_OK;
}

/* Has the hardware finished everything handed to it? */
bool audio_drained(uint32_t pid)
{
    if (!ac97_present() || !g_au.open || g_au.owner_pid != pid) return true;
    if (!g_au.started) {
        /* Asking whether it drained is the writer saying it has stopped
         * writing -- so a sound shorter than the prefill threshold plays HERE
         * rather than never. Without this a 50ms beep is silent. */
        if (g_au.queued > 0) { ac97_play(g_au.last); g_au.started = true; g_au.last_civ = -1; return false; }
        return true;
    }
    position_poll();
    return ac97_done(g_au.last);
}

void audio_close(uint32_t pid)
{
    if (!g_au.open || g_au.owner_pid != pid) return;
    /* What the stream actually carried, on the serial log. The WAV says what
     * came OUT; this says what went IN, and the difference between them is the
     * only way to tell a writer that gave up from a device that stopped. */
    kprintf("audio: stream closed after %llu frames (%llu ms)\n",
            (unsigned long long)g_au.frames_written,
            (unsigned long long)(g_au.frames_written * 1000 /
                                 (ac97_sample_rate() ? ac97_sample_rate() : 1)));
    ac97_stop();
    g_au.open = false;
    g_au.started = false;
    g_au.owner_pid = 0;
}

/* Called when a process dies: a program that exits mid-note must not leave the
 * speaker running and the device claimed. The compositor learned this lesson
 * with windows (compositor_reap_pid); sound is the same shape. */
void audio_reap_pid(uint32_t pid)
{
    if (g_au.open && g_au.owner_pid == pid) {
        kprintf("audio: reclaiming the device from pid %u\n", pid);
        audio_close(pid);
    }
}
