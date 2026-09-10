/* tonestress.c -- how SHALLOW can the audio buffer be while the machine is busy?
 *
 * The first version of this asked the obvious question -- does a busy machine
 * make the speaker run dry -- and the answer was no, zero underruns under
 * round-robin, because the driver keeps roughly 320 ms of audio queued ahead
 * of the device. Nothing a scheduler does on a 10 ms tick can reach through
 * 320 ms of runway. Measuring underruns at that depth measures the buffer, not
 * the scheduler.
 *
 * THE REAL COST OF A BUFFER IS LATENCY. Every millisecond queued ahead is a
 * millisecond between a program deciding to make a sound and the sound
 * existing. A game, a synth, a video player syncing to picture -- all of them
 * are paying for the scheduler's unreliability in delay a person can feel. So
 * the question worth asking is not "does it underrun" but "how little runway
 * can it keep and still not underrun", because that number IS the achievable
 * latency.
 *
 * So this writer paces itself by TIME rather than filling the ring: it keeps
 * exactly `depth` milliseconds of audio ahead of the playback position and no
 * more. Sweep `depth` down until it breaks; where it breaks is the answer, and
 * the two policies can be compared on it.
 *
 *   tonestress [hogs] [ms] [declare] [depth_ms]
 *   default: 6 hogs, 1500 ms, declare 1, 120 ms of runway
 *
 * `declare` says whether to call embk_sched_period() -- so the same binary is
 * both halves of the comparison, and the difference between two runs cannot be
 * a difference between two programs.
 *
 * TWO THINGS HAD TO BE FIXED BEFORE THIS COULD MEASURE ANYTHING, and both
 * were real gaps rather than test scaffolding:
 *
 *   The position had to come from the DEVICE. Pacing off the wall clock is
 *   wrong because the device does not start when you start writing -- it
 *   starts once the prefill is met -- so a clock-based estimate is ahead of
 *   the truth by exactly the prefill, and this test silently held 170 ms of
 *   runway while asking for 95. embk_audio_position() is the fix and is what
 *   A/V sync needs anyway.
 *
 *   The driver would not start below ~170 ms of queued audio. That prefill is
 *   the latency floor, and no scheduler can get under it, so it had to become
 *   something a writer can lower: embk_audio_latency().
 *
 * Neither half is worth anything alone. A scheduler that wakes you on time
 * cannot help if the driver insists on 170 ms of buffer; a shallow buffer
 * cannot help if you are not woken in time to refill it.
 *
 * Exit code: 0 if the tone was written in full, 1 if the device refused, 2 if
 * a hog could not start, 3 if the writer gave up on a device that had stopped
 * consuming. The underrun count is NOT in the exit code -- it belongs to the
 * kernel, which is the only side that can see it honestly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "embk.h"

#define CHUNK_FRAMES 1024
#define MAX_HOGS     16

static volatile int g_stop;
/* Where the hogs' arithmetic goes. `volatile` on the accumulator is not enough
 * to keep the compiler from noticing nothing reads it; this is the read. */
static volatile unsigned g_sink;
static volatile int g_running;

static void hog(long arg) {
    (void)arg;
    __atomic_fetch_add(&g_running, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) {
        volatile unsigned x = 0;
        for (int i = 0; i < 20000; i++) x += (unsigned)i;
        g_sink = x;
    }
    embk_thread_exit(0);
}

int main(int argc, char **argv)
{
    int hogs    = argc > 1 ? atoi(argv[1]) : 6;
    int ms      = argc > 2 ? atoi(argv[2]) : 1500;
    int declare = argc > 3 ? atoi(argv[3]) : 1;
    int depth   = argc > 4 ? atoi(argv[4]) : 120;

    if (hogs < 0) hogs = 0;
    if (hogs > MAX_HOGS) hogs = MAX_HOGS;
    if (ms < 100) ms = 100;
    if (depth < 10) depth = 10;
    if (depth > 1000) depth = 1000;

    uint32_t rate = embk_audio_rate();
    if ((int)rate <= 0) {
        fprintf(stderr, "tonestress: no audio device (or no `audio` capability)\n");
        return 1;
    }
    if (embk_audio_open() < 0) {
        fprintf(stderr, "tonestress: cannot open the audio device\n");
        return 1;
    }

    /* Ask the device to start as soon as we have the runway we intend to keep.
     * Without this the driver holds ~170 ms before making a sound, which IS
     * the latency and is below nothing this test could measure. */
    int granted = embk_audio_latency((uint32_t)depth);
    if (granted < 0) granted = 0;

    /* THE PERIOD IS THE REFILL INTERVAL, and the budget is what a refill
     * costs. 10 ms is well inside the runway the driver keeps queued, so this
     * is not asking to be woken as often as possible -- it is asking to be
     * woken often enough that the runway never reaches zero. The budget is
     * small because generating a square wave and memcpy-ing it is small; the
     * whole point of declaring one honestly is that it leaves the rest of the
     * machine admissible. */
    int decl = 0;
    if (declare)
        decl = embk_sched_period(10, 3);

    printf("tonestress: %d hog(s), %d ms of tone, %d ms runway (granted %d), "
           "declare=%d -> %d\n", hogs, ms, depth, granted, declare, decl);
    fflush(stdout);

    for (int i = 0; i < hogs; i++) {
        if (embk_thread_create(hog, i) < 0) {
            fprintf(stderr, "tonestress: could not start hog %d\n", i);
            embk_audio_close();
            return 2;
        }
    }
    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST) < hogs)
        embk_sleep_ms(1);

    uint32_t total  = (uint32_t)((uint64_t)rate * (uint32_t)ms / 1000);
    uint32_t period = rate / 440;
    if (period < 2) period = 2;

    static int16_t chunk[CHUNK_FRAMES * 2];
    uint32_t done = 0, spins = 0;
    int rc = 0;

    /* How many frames of runway to hold. `granted` rather than `depth` when the
     * driver rounded up: it will not start until that much is queued, so a
     * writer holding less than it would be waiting for a device waiting for
     * it. Hold what was actually granted. */
    int held = granted > depth ? granted : depth;
    const uint32_t runway = (uint32_t)((uint64_t)rate * (uint32_t)held / 1000);

    /* CHUNK SIZE IS PART OF THE LATENCY, not an implementation detail. Each
     * write becomes one descriptor and the device stops at the end of it, so a
     * writer that hands over 21 ms at a time cannot have a buffer shallower
     * than two of those however little runway it asks for. A third of the
     * runway gives three descriptors in flight, which is enough for the device
     * always to have a next one and few enough to be honest about the depth. */
    uint32_t chunk_n = runway / 3;
    if (chunk_n < 64)           chunk_n = 64;
    if (chunk_n > CHUNK_FRAMES) chunk_n = CHUNK_FRAMES;

    while (done < total) {
        /* Where the speaker is NOW, from the device rather than from the
         * clock. Stay `runway` ahead and no further: writing more is what the
         * first version of this did, and it made the measurement about the
         * ring size instead of about the scheduler.
         *
         * ZERO MEANS NOT STARTED, not "at the beginning". Before the device
         * begins there is no position to pace against and the only thing that
         * makes it begin is more queued audio -- so throttling on a position
         * of 0 is a writer and a device each waiting for the other. Until it
         * starts, the ring-full path below is the throttle. */
        uint64_t played = embk_audio_position();
        if (played > 0 && done > played + runway) {
            embk_sleep_ms(2);
            continue;
        }

        uint32_t n = total - done;
        if (n > chunk_n) n = chunk_n;

        for (uint32_t f = 0; f < n; f++) {
            int16_t v = ((done + f) % period) < (period / 2) ? 8000 : -8000;
            chunk[f * 2 + 0] = v;
            chunk[f * 2 + 1] = v;
        }

        int took = embk_audio_write(chunk, n);
        if (took < 0) { rc = 1; break; }
        if (took == 0) {
            /* The ring is full even though we are inside our own runway --
             * which happens when `depth` is deeper than the ring can hold.
             * Sleeping here is what makes the declared period mean something;
             * a spin would take the CPU the refill is competing for. */
            if (++spins > 20000) { rc = 3; break; }
            embk_sleep_ms(2);
            continue;
        }
        spins = 0;
        done += (uint32_t)took;
    }

    for (int i = 0; i < 5000 && !embk_audio_drained(); i++) embk_sleep_ms(2);

    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    embk_audio_close();
    printf("tonestress: wrote %u of %u frames\n", done, total);
    embk_sleep_ms(30);
    return rc;
}
