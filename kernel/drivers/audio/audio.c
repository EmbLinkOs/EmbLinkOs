/* kernel/drivers/audio/audio.c -- one PCM sink, and who is allowed to use it.
 *
 * The device-independent half. ac97.c and hda.c know about ports, descriptor
 * lists and codec graphs; this knows about a stream: who owns it, how far the
 * hardware has got, and what to do when a program writes faster or slower than
 * the speaker consumes. Which driver is underneath is decided at BOOT, by
 * probing, not at build time -- see pcm.h for why that changed.
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
#include "include/kstring.h"
#include "drivers/audio/pcm.h"
#include "drivers/audio/audio.h"

/* WHICH SOUND CARD THIS MACHINE HAS, answered by asking rather than by
 * choosing at build time. Order is preference where a machine has more than
 * one: HDA first because it is what hardware built after about 2008 actually
 * has, and a board that carries both (some transitional chipsets did) should
 * use the modern one.
 *
 * ac97.c is x86-only -- its registers are I/O PORTS, an instruction that does
 * not exist on aarch64 -- and virtio-snd is what the ARM machine is given, so
 * the list genuinely differs per architecture. That is a fact about the
 * hardware, not a leftover of the old build-time seam: every driver that CAN
 * run here is here, and the choice between them is made at runtime. */
static const struct pcm_driver *const g_drivers[] = {
#if defined(__x86_64__)
    &hda_driver,
    &ac97_driver,
#else
    &virtio_snd_driver,
#endif
};

static const struct pcm_driver *g_dev;   /* NULL until one of them probes OK */

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
    uint32_t stage_n;             /* frames held back, short of a descriptor  */

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

/* HOLDING BACK A PARTIAL DESCRIPTOR, and why only on some devices.
 *
 * AC'97 writes the true length into each descriptor, so a 704-frame fill
 * plays 704 frames. HDA's engine walks a buffer whose TOTAL size is fixed
 * before it starts, so a short fill is PADDED with silence and the silence is
 * played. A writer whose chunk is not a whole descriptor therefore gets a
 * hole after every single write, and the size of the hole is the size of the
 * remainder.
 *
 * THE NUMBER, because this is not a plausible-sounding worry. `tonestress`
 * asked for a 20 ms buffer, which makes it write 320 frames at a time against
 * a 1024-frame descriptor. On HDA, without this:
 *
 *     400 ms of tone came out as 1.27 s, 32% non-silent, measured 139.5 Hz
 *
 * -- a third of the sound, stretched to three times its length, two octaves
 * flat. With it, the same run: 0.40 s, 100% non-silent, 441.2 Hz.
 *
 * So on a device that pads, the remainder is not handed over. It is kept here
 * and goes out with the front of the next write, and the writer's chunk size
 * stops being audible. The frames are ACCEPTED when they are staged -- the
 * caller must not send them again -- and the only descriptor that is ever
 * padded is the last one of a sound, flushed by audio_drained(), where the
 * padding is silence at the end of something that has ended.
 *
 * `desc_frames` is the flag, because it is the same property: a driver needs
 * that op exactly when what it plays is not what was put in. */
#define STAGE_FRAMES 1024
static int16_t g_stage[STAGE_FRAMES * 2];

static bool device_pads(void)
{
    return g_dev && g_dev->desc_frames != NULL &&
           g_dev->frames_per_buffer() <= STAGE_FRAMES;
}

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
    int civ = (int)g_dev->civ();
    if (g_au.last_civ < 0) { g_au.last_civ = civ; return; }
    /* Bounded by RING: an unbounded walk on a garbage CIV would hang here, and
     * a device that reported one has failed in a way this cannot fix. */
    for (int guard = 0; guard < RING && g_au.last_civ != civ; guard++) {
        g_au.played += g_au.desc_frames[g_au.last_civ];
        g_au.last_civ = (g_au.last_civ + 1) % RING;
    }
}

/* PROBE, in order, and keep the first that finds its hardware. Called once
 * during boot, from the architecture's device bring-up. */
bool audio_init(void)
{
    for (unsigned i = 0; i < sizeof g_drivers / sizeof g_drivers[0]; i++) {
        if (g_drivers[i]->init()) {
            g_dev = g_drivers[i];
            kprintf("audio: using the %s driver\n", g_dev->name);
            return true;
        }
    }
    kprintf("audio: no sound card this kernel has a driver for\n");
    return false;
}

const char *audio_device_name(void) { return g_dev ? g_dev->name : "none"; }

bool audio_available(void)
{
    return g_dev != NULL;
}

/* THE DEVICE, NOT THE STREAM. `test audio` plays a tone with no process
 * owning anything, which is the only way to tell a broken driver from a
 * broken stream layer -- so the raw operations stay reachable. */
uint32_t audio_dev_fill(int i, const int16_t *f, uint32_t n)
{
    return g_dev ? g_dev->fill(i, f, n) : 0;
}
void audio_dev_play(int last) { if (g_dev) g_dev->play(last); }
bool audio_dev_done(int last) { return g_dev ? g_dev->done(last) : true; }
void audio_dev_stop(void)     { if (g_dev) g_dev->stop(); }

uint32_t audio_sample_rate(void)
{
    return g_dev ? g_dev->sample_rate() : 0;
}

uint32_t audio_frames_per_buffer(void)
{
    return g_dev ? g_dev->frames_per_buffer() : 0;
}

int audio_open(uint32_t pid)
{
    if (!g_dev) return -EMBK_ENODEV;
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
    g_au.stage_n = 0;
    for (int i = 0; i < RING; i++) g_au.desc_frames[i] = 0;

    /* The default prefill, expressed in frames rather than descriptors now
     * that a descriptor is not a fixed size. Same duration as before. */
    g_au.start_frames = (uint32_t)RING_START_MIN * g_dev->frames_per_buffer();
    return EMBK_OK;
}

int audio_set_latency(uint32_t pid, uint32_t ms)
{
    if (!g_dev) return -EMBK_ENODEV;
    if (!g_au.open || g_au.owner_pid != pid) return -EMBK_EPERM;
    /* Only before the device is running. Lowering the prefill of a stream that
     * has already started means nothing (it started), and raising it means
     * nothing either -- so a call that arrives late is a caller who thinks
     * something happened that did not. */
    if (g_au.started) return -EMBK_EBUSY;

    uint32_t rate = g_dev->sample_rate();
    if (!rate) return -EMBK_ENODEV;

    /* THE FLOOR IS TWO DESCRIPTORS' WORTH OF WHATEVER THE WRITER SENDS, and it
     * cannot be expressed in frames here because the writer has not sent
     * anything yet. So the floor enforced here is a duration -- below
     * AUDIO_MIN_LATENCY_MS the device would reach the end of the list inside a
     * single scheduling round on any machine, and granting it would be
     * promising something no scheduler can keep. */
    if (ms < AUDIO_MIN_LATENCY_MS) ms = AUDIO_MIN_LATENCY_MS;

    uint64_t frames = (uint64_t)rate * ms / 1000;
    uint32_t cap    = (uint32_t)(RING - RING_HEADROOM) * g_dev->frames_per_buffer();
    if (frames > cap) frames = cap;
    g_au.start_frames = (uint32_t)frames;

    /* What it actually got, in ms -- the caller asked in a unit the hardware
     * does not have, and the difference is theirs to know. */
    return (int)((uint64_t)g_au.start_frames * 1000 / rate);
}

uint64_t audio_position(uint32_t pid)
{
    if (!g_dev || !g_au.open || g_au.owner_pid != pid) return 0;
    if (!g_au.started) return 0;
    position_poll();
    return g_au.played;
}

void audio_stats(uint64_t *underruns, uint64_t *frames, uint64_t *writes)
{
    if (underruns) *underruns = g_dev ? g_dev->underruns() : 0;
    if (frames)    *frames    = g_au.frames_written;
    if (writes)    *writes    = g_au.writes;
}

/* Record a descriptor the driver has just accepted. */
static void commit(uint32_t n)
{
    /* How long this descriptor will PLAY, remembered for position_poll().
     * Usually that is what fill() took -- but not on a device whose
     * descriptors are a fixed size and whose short fills are padded with
     * silence, and the padding is time the speaker spends. Asking the driver
     * is the difference between a position that is right and one that drifts
     * by the padding on every short write. */
    g_au.desc_frames[g_au.head] =
        g_dev->desc_frames ? g_dev->desc_frames(g_au.head) : n;
    g_au.last = g_au.head;
    g_au.head = (g_au.head + 1) % RING;
    g_au.queued++;
    if (!g_au.started) g_au.queued_frames += n;
}

/* How many descriptors are free to write into right now. */
static int ring_free(void)
{
    if (!g_au.started) return RING - RING_HEADROOM;

    int civ = (int)g_dev->civ();
    int used = g_au.head - civ;
    if (used < 0) used += RING;
    int freed = RING - used - RING_HEADROOM;
    return freed > 0 ? freed : 0;
}

int audio_write(uint32_t pid, const int16_t *frames, uint32_t nframes,
                uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!g_dev) return -EMBK_ENODEV;
    if (!g_au.open || g_au.owner_pid != pid) return -EMBK_EPERM;
    if (frames == NULL) return -EMBK_EINVAL;

    position_poll();
    uint32_t taken = 0;
    int slots = ring_free();

    const bool stage = device_pads();
    const uint32_t percb = g_dev->frames_per_buffer();

    while (slots > 0 && taken < nframes) {
        uint32_t n;
        if (stage) {
            /* Top the held descriptor up from the caller's frames, and hand
             * it over only when it is FULL. A short one goes out only from
             * audio_drained(), where it is the end of the sound. */
            uint32_t room = percb - g_au.stage_n;
            uint32_t have = nframes - taken;
            uint32_t cp   = room < have ? room : have;
            memcpy(g_stage + (size_t)g_au.stage_n * 2,
                   frames + (size_t)taken * 2, (size_t)cp * 4);
            g_au.stage_n += cp;
            taken += cp;
            if (g_au.stage_n < percb) break;      /* accepted, not committed */
            n = g_dev->fill(g_au.head, g_stage, g_au.stage_n);
            if (n == 0) { taken -= cp; g_au.stage_n -= cp; break; }
            g_au.stage_n = 0;
        } else {
            n = g_dev->fill(g_au.head, frames + (size_t)taken * 2,
                            nframes - taken);
            if (n == 0) break;
            taken += n;
        }
        commit(n);
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
                g_dev->play(g_au.last);
                g_au.started = true;
                g_au.last_civ = -1;
            }
        } else {
            g_dev->set_last(g_au.last);
        }
    }

    if (accepted) *accepted = taken;
    /* Short writes are NORMAL and are not an error: the ring is full and the
     * caller should come back. Saying EAGAIN for a partial accept would make
     * every well-behaved writer look like it failed. */
    return EMBK_OK;
}

/* Hand over a descriptor that is still short, because nothing more is coming.
 * False means the ring has no room yet and the caller must come back. */
static bool stage_flush(void)
{
    if (g_au.stage_n == 0) return true;
    if (ring_free() <= 0) return false;
    uint32_t n = g_dev->fill(g_au.head, g_stage, g_au.stage_n);
    if (n == 0) return false;
    g_au.stage_n = 0;
    commit(n);
    return true;
}

/* Has the hardware finished everything handed to it? */
bool audio_drained(uint32_t pid)
{
    if (!g_dev || !g_au.open || g_au.owner_pid != pid) return true;

    /* ASKING THIS IS THE WRITER SAYING IT HAS STOPPED, which is the one thing
     * that makes a partial descriptor final. Out it goes -- padded, on a
     * device that pads -- because the alternative is that the tail of every
     * sound is silently dropped. */
    bool held = g_au.stage_n > 0;
    if (!stage_flush()) return false;
    if (held && g_au.started) g_dev->set_last(g_au.last);

    if (!g_au.started) {
        /* Asking whether it drained is the writer saying it has stopped
         * writing -- so a sound shorter than the prefill threshold plays HERE
         * rather than never. Without this a 50ms beep is silent. */
        if (g_au.queued > 0) { g_dev->play(g_au.last); g_au.started = true; g_au.last_civ = -1; return false; }
        return true;
    }
    position_poll();
    return g_dev->done(g_au.last);
}

void audio_close(uint32_t pid)
{
    if (!g_au.open || g_au.owner_pid != pid) return;
    /* What the stream actually carried, on the serial log. The WAV says what
     * came OUT; this says what went IN, and the difference between them is the
     * only way to tell a writer that gave up from a device that stopped. */
    kprintf("audio: stream closed after %llu frames (%llu ms), %llu underrun(s)\n",
            (unsigned long long)g_au.frames_written,
            (unsigned long long)(g_au.frames_written * 1000 /
                                 (g_dev->sample_rate() ? g_dev->sample_rate() : 1)),
            (unsigned long long)g_dev->underruns());
    g_dev->stop();
    g_au.open = false;
    g_au.started = false;
    g_au.stage_n = 0;
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
