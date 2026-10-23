/* kernel/drivers/audio/audio.c -- one PCM sink, and who is allowed to use it.
 *
 * The device-independent half. ac97.c knows about I/O ports and descriptor
 * lists; this knows about a stream: who owns it, how far the hardware has got,
 * and what to do when a program writes faster or slower than the speaker
 * consumes. The adapter helpers select HDA or AC'97 without changing the
 * syscall PCM contract.
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
#include "drivers/audio/hda.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "include/kstring.h"
static spinlock_t audio_mutex=SPINLOCK_INIT;
static unsigned using_hda,volume=100,muted;
static bool device_present(void) {return using_hda?hda_present():ac97_present();}
static uint32_t device_rate(void) {return using_hda?48000:ac97_sample_rate();}
static uint32_t device_frames(void) {return using_hda?1024:ac97_frames_per_buffer();}
static uint32_t device_fill(int i,const int16_t *p,uint32_t n) {return using_hda?hda_fill(i,p,n):ac97_fill(i,p,n);}
static unsigned device_civ(void) {return using_hda?hda_civ():ac97_civ();}
static void device_play(int i) {if(using_hda) hda_play(i);else ac97_play(i);}
static void device_last(int i) {if(using_hda) hda_set_last(i);else ac97_set_last(i);}
static bool device_done(int i) {return using_hda?hda_done(i):ac97_done(i);}
static void device_stop(void) {if(using_hda) hda_stop();else ac97_stop();}

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
 * the difference between a stream and a series of one-shots. */
#define RING_START_MIN 8

static struct {
    bool     open;
    uint32_t owner_pid;
    int      head;                /* next descriptor to fill                  */
    int      last;                /* highest index handed to the hardware     */
    int      queued;              /* descriptors filled but not yet started   */
    bool     started;
    uint64_t frames_written;
} g_au;

bool audio_available(void)
{
    return device_present();
}

uint32_t audio_sample_rate(void)
{
    return device_present()?device_rate():0;
}

uint32_t audio_frames_per_buffer(void)
{
    return device_frames();
}

static int audio_open_locked(uint32_t pid)
{
    if (!device_present()) return -EMBK_ENODEV;
    /* Re-opening by the SAME process is not an error -- a program that
     * restarts its own playback should not have to know whether it closed. */
    if (g_au.open && g_au.owner_pid != pid) return -EMBK_EBUSY;

    if (g_au.open) return 0;
    device_stop();
    g_au.open = true;
    g_au.owner_pid = pid;
    g_au.head = 0;
    g_au.last = -1;
    g_au.queued = 0;
    g_au.started = false;
    g_au.frames_written = 0;
    return EMBK_OK;
}

/* How many descriptors are free to write into right now. */
static int ring_free(void)
{
    if (!g_au.started) return RING - RING_HEADROOM;

    int civ = (int)device_civ();
    int used = g_au.head - civ;
    if (used < 0) used += RING;
    int freed = RING - used - RING_HEADROOM;
    return freed > 0 ? freed : 0;
}

static int audio_write_locked(uint32_t pid, const int16_t *frames, uint32_t nframes,
                uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!device_present()) return -EMBK_ENODEV;
    if (!g_au.open || g_au.owner_pid != pid) return -EMBK_EPERM;
    if (frames == NULL) return -EMBK_EINVAL;

    uint32_t taken = 0;
    int slots = ring_free();

    while (slots > 0 && taken < nframes) {
        int16_t stage[2048];
        uint32_t count=nframes-taken;if(count>1024) count=1024;
        unsigned gain=muted?0:volume;
        for(unsigned i=0;i<count*2;i++) stage[i]=(int32_t)frames[taken*2+i]*(int)gain/100;
        uint32_t n = device_fill(g_au.head,stage,count);
        if (n == 0) break;
        taken += n;
        g_au.last = g_au.head;
        g_au.head = (g_au.head + 1) % RING;
        g_au.queued++;
        slots--;
    }

    if (taken > 0) {
        g_au.frames_written += taken;
        /* Start on the FIRST write, extend on every one after. Starting from
         * audio_open would run the device over empty descriptors and put a
         * burst of silence at the head of every sound. */
        if (!g_au.started) {
            /* Hold until there is enough queued to keep the device fed. A
             * writer with less than this in total gets it started by
             * audio_drained_locked(), which is what "I have finished writing" means
             * from out here. */
            if (g_au.queued >= RING_START_MIN) {
                device_play(g_au.last);
                g_au.started = true;
            }
        } else {
            device_last(g_au.last);
        }
    }

    if (accepted) *accepted = taken;
    /* Short writes are NORMAL and are not an error: the ring is full and the
     * caller should come back. Saying EAGAIN for a partial accept would make
     * every well-behaved writer look like it failed. */
    return EMBK_OK;
}

/* Has the hardware finished everything handed to it? */
static bool audio_drained_locked(uint32_t pid)
{
    if (!device_present() || !g_au.open || g_au.owner_pid != pid) return true;
    if (!g_au.started) {
        /* Asking whether it drained is the writer saying it has stopped
         * writing -- so a sound shorter than the prefill threshold plays HERE
         * rather than never. Without this a 50ms beep is silent. */
        if (g_au.queued > 0) { device_play(g_au.last); g_au.started = true; return false; }
        return true;
    }
    return device_done(g_au.last);
}

static void audio_close_locked(uint32_t pid)
{
    if (!g_au.open || g_au.owner_pid != pid) return;
    /* What the stream actually carried, on the serial log. The WAV says what
     * came OUT; this says what went IN, and the difference between them is the
     * only way to tell a writer that gave up from a device that stopped. */
    kprintf("audio: stream closed after %llu frames (%llu ms)\n",
            (unsigned long long)g_au.frames_written,
            (unsigned long long)(g_au.frames_written * 1000 /
                                 (device_rate() ? device_rate() : 1)));
    device_stop();
    g_au.open = false;
    g_au.started = false;
    g_au.owner_pid = 0;
}

/* Called when a process dies: a program that exits mid-note must not leave the
 * speaker running and the device claimed. The compositor learned this lesson
 * with windows (compositor_reap_pid); sound is the same shape. */
static void audio_reap_pid_locked(uint32_t pid)
{
    if (g_au.open && g_au.owner_pid == pid) {
        kprintf("audio: reclaiming the device from pid %u\n", pid);
        audio_close_locked(pid);
    }
}

void audio_init(void) {
    spinlock_init(&audio_mutex);hda_init();using_hda=hda_present();if(!using_hda) ac97_init();
}
int audio_open(uint32_t pid) {spin_lock(&audio_mutex);int rc=audio_open_locked(pid);spin_unlock(&audio_mutex);return rc;}
int audio_write(uint32_t pid,const int16_t *p,uint32_t n,uint32_t *accepted) {
    spin_lock(&audio_mutex);int rc=audio_write_locked(pid,p,n,accepted);spin_unlock(&audio_mutex);return rc;
}
bool audio_drained(uint32_t pid) {spin_lock(&audio_mutex);bool done=audio_drained_locked(pid);spin_unlock(&audio_mutex);return done;}
void audio_close(uint32_t pid) {spin_lock(&audio_mutex);audio_close_locked(pid);spin_unlock(&audio_mutex);}
void audio_reap_pid(uint32_t pid) {spin_lock(&audio_mutex);audio_reap_pid_locked(pid);spin_unlock(&audio_mutex);}
void audio_snapshot(struct embk_audio_info *out) {
    spin_lock(&audio_mutex);memset(out,0,sizeof *out);out->version=EMBK_AUDIO_VERSION;
    if(device_present()) {out->flags|=EMBK_AUDIO_AVAILABLE;out->sample_rate=device_rate();
        const char *name=using_hda?hda_output_name():"AC'97 stereo output";strncpy(out->output,name,sizeof out->output-1);}
    if(g_au.open) out->flags|=EMBK_AUDIO_PLAYING;
    if(muted) out->flags|=EMBK_AUDIO_MUTED;
    out->percent=volume;spin_unlock(&audio_mutex);
}
int audio_control(unsigned percent,unsigned mute) {
    if(percent>100 || mute>1) return -EMBK_EINVAL;
    spin_lock(&audio_mutex);int rc=0;
    if(!device_present()) rc=-EMBK_ENODEV;else {volume=percent;muted=mute;}
    spin_unlock(&audio_mutex);return rc;
}
