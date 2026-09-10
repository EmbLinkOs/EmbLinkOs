#include "drivers/audio/ac97.h"
#include "drivers/bus/virtio_pci.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/pmm.h"

/* virtio-snd -- sound on a machine with no AC'97 codec.
 *
 * WHY IT IMPLEMENTS THE ac97_* NAMES. Those nine functions are not "the AC'97
 * driver's interface", they are THE AUDIO DRIVER INTERFACE this kernel has:
 * kernel/drivers/audio/audio.c -- the shared mixer, ring accounting and
 * per-process ownership -- is written against them, and it is 190 lines that
 * work. Renaming them to something machine-neutral would be a nicer-looking
 * change that moved no logic; implementing them is a driver. The same choice
 * virtio-input made with keyboard.c, for the same reason.
 *
 * THE TWO DEVICES DO NOT AGREE ON SHAPE, and the mapping is where the work is:
 *
 *   AC'97                            virtio-snd
 *   32 descriptors in a ring         a TX virtqueue
 *   the device walks them            the driver pushes buffers
 *   CIV = which one is playing       a used ring of completions
 *   LVI = last valid, extend to run  submit more, or the stream underruns
 *
 * So this keeps AC'97's 32-buffer ring as its own storage, and treats "extend
 * the valid range" as "submit everything not yet submitted". CIV is derived
 * from completions: the oldest buffer still in flight IS the one playing.
 *
 * Setup is a conversation on the CONTROL queue -- SET_PARAMS, PREPARE, START --
 * which AC'97 does with three port writes. Each is a small request/response
 * pair, and each is checked: a stream that was never PREPAREd accepts buffers
 * and plays silence, which is the failure worth catching at boot.
 */

#define VIRTIO_SND_DEVID    0x1059      /* 0x1040 + device type 25 (sound) */

/* Queues, in the order the spec fixes them. */
#define VQ_CONTROL  0
#define VQ_EVENT    1
#define VQ_TX       2
#define VQ_RX       3

/* Control request codes. */
#define VIRTIO_SND_R_PCM_INFO        0x0100
#define VIRTIO_SND_R_PCM_SET_PARAMS  0x0101
#define VIRTIO_SND_R_PCM_PREPARE     0x0102
#define VIRTIO_SND_R_PCM_RELEASE     0x0103
#define VIRTIO_SND_R_PCM_START       0x0104
#define VIRTIO_SND_R_PCM_STOP        0x0105
#define VIRTIO_SND_S_OK              0x8000

/* PCM formats and rates, as the spec's ENUMS number them -- these are indices
 * into a list, not bit positions and not the rate in Hz. Getting them wrong
 * produces a clean, correct-looking refusal from the device (SET_PARAMS
 * returns not-OK) with nothing to say which field it disliked. */
#define VIRTIO_SND_PCM_FMT_S16       5   /* after IMA_ADPCM, MU_LAW, A_LAW, S8, U8 */
#define VIRTIO_SND_PCM_RATE_44100    6   /* after 5512, 8000, 11025, 16000, 22050, 32000 */

#define RATE_HZ        44100u
#define CHANNELS       2u
#define FRAMES_PER_BUF 1024u
#define BUF_BYTES      (FRAMES_PER_BUF * CHANNELS * sizeof(int16_t))
#define NBUFS          32              /* must match audio.c's RING */

struct snd_hdr { uint32_t code; } __attribute__((packed));

struct pcm_set_params {
    uint32_t hdr;          /* VIRTIO_SND_R_PCM_SET_PARAMS */
    uint32_t stream_id;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
} __attribute__((packed));

struct pcm_hdr {
    uint32_t hdr;          /* the request code */
    uint32_t stream_id;
} __attribute__((packed));

struct pcm_xfer  { uint32_t stream_id; } __attribute__((packed));
struct pcm_status { uint32_t status; uint32_t latency_bytes; } __attribute__((packed));

/* --- rings ---------------------------------------------------------------
 * In .bss, not the heap: a descriptor addresses PHYSICAL memory and KV2P()
 * only works on the kernel image window. Same constraint every other virtio
 * driver here records. */
#define CQ_SIZE 8
#define TQ_SIZE 64

struct avail_c { uint16_t flags, idx; uint16_t ring[CQ_SIZE]; } __attribute__((packed));
struct used_c  { uint16_t flags, idx; struct vring_used_elem ring[CQ_SIZE]; } __attribute__((packed));
struct avail_t { uint16_t flags, idx; uint16_t ring[TQ_SIZE]; } __attribute__((packed));
struct used_t  { uint16_t flags, idx; struct vring_used_elem ring[TQ_SIZE]; } __attribute__((packed));

static struct vring_desc cdesc[CQ_SIZE] __attribute__((aligned(16)));
static struct avail_c    cavail          __attribute__((aligned(2)));
static struct used_c     cused           __attribute__((aligned(4)));

static struct vring_desc tdesc[TQ_SIZE] __attribute__((aligned(16)));
static struct avail_t    tavail          __attribute__((aligned(2)));
static struct used_t     tused           __attribute__((aligned(4)));

/* One control request/response pair, reused: the control queue is used only at
 * setup and teardown, one call at a time. */
static uint8_t         creq[64] __attribute__((aligned(16)));
static struct snd_hdr  cresp    __attribute__((aligned(16)));

/* The audio ring AC'97's interface describes, as real storage. */
static int16_t        g_buf[NBUFS][FRAMES_PER_BUF * CHANNELS] __attribute__((aligned(16)));
static uint32_t       g_frames[NBUFS];          /* frames actually in each     */
static struct pcm_xfer   g_xfer[NBUFS]  __attribute__((aligned(16)));
static struct pcm_status g_stat[NBUFS]  __attribute__((aligned(16)));

static struct virtio_pci_dev g_vd;
static bool     g_up;
static bool     g_running;
static uint16_t g_cnotify, g_tnotify;
static uint16_t g_cqsize, g_tqsize;
static uint16_t g_tlast_used;                   /* our cursor into tused       */
static uint32_t g_submitted;                    /* buffers pushed, ever        */
static uint32_t g_completed;                    /* buffers retired, ever       */
static int      g_next_submit;                  /* ring index not yet pushed   */

static inline uint64_t dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

/* Send one control request and wait for its response. Polled: this happens
 * three times at boot and twice at teardown, never on the audio path. */
static bool ctl(const void *req, uint32_t reqlen) {
    memcpy(creq, req, reqlen);
    cresp.code = 0;

    cdesc[0].addr  = dma(creq);
    cdesc[0].len   = reqlen;
    cdesc[0].flags = VRING_DESC_F_NEXT;
    cdesc[0].next  = 1;
    cdesc[1].addr  = dma(&cresp);
    cdesc[1].len   = sizeof cresp;
    cdesc[1].flags = VRING_DESC_F_WRITE;
    cdesc[1].next  = 0;

    uint16_t slot = cavail.idx % g_cqsize;
    cavail.ring[slot] = 0;
    __sync_synchronize();
    cavail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_cnotify, VQ_CONTROL);

    static uint16_t last_used;
    for (uint64_t spins = 0; spins < 200000000ULL; spins++) {
        __sync_synchronize();
        if (cused.idx != last_used) {
            last_used = cused.idx;
            return cresp.code == VIRTIO_SND_S_OK;
        }
        __asm__ volatile("yield" ::: "memory");
    }
    kprintf("virtio-snd: control request timed out\n");
    return false;
}

static bool pcm_simple(uint32_t code) {
    struct pcm_hdr h = { code, 0 };
    return ctl(&h, sizeof h);
}

/* Retire everything the device has finished with. Called from the query
 * functions, which is where audio.c naturally asks. */
static void reap(void) {
    __sync_synchronize();
    while (g_tlast_used != tused.idx) {
        g_tlast_used++;
        g_completed++;
    }
}

/* --- the ac97_* interface ------------------------------------------------ */

bool ac97_present(void) { return g_up; }

uint32_t ac97_sample_rate(void) { return g_up ? RATE_HZ : 0; }

uint32_t ac97_frames_per_buffer(void) { return FRAMES_PER_BUF; }

uint32_t ac97_fill(int i, const int16_t *frames, uint32_t nframes) {
    if (!g_up || i < 0 || i >= NBUFS || !frames)
        return 0;

    uint32_t take = nframes < FRAMES_PER_BUF ? nframes : FRAMES_PER_BUF;
    memcpy(g_buf[i], frames, take * CHANNELS * sizeof(int16_t));
    g_frames[i] = take;
    return take;
}

/* "Which descriptor is playing now."
 *
 * virtio-snd has no such register, so it is DERIVED: buffers retire in order,
 * so the oldest one still in flight is the one being played. With none in
 * flight the honest answer is where the next submission will go. */
uint8_t ac97_civ(void) {
    if (!g_up) return 0;
    reap();
    return (uint8_t)(g_completed % NBUFS);
}

/* Push every buffer from the last submission through `last`, inclusive.
 *
 * AC'97 separates "start playing" (play) from "extend the valid range"
 * (set_last) because the device walks descriptors on its own and has to be
 * told where to stop. virtio-snd has no such distinction -- a buffer that has
 * been submitted will be played and one that has not will not -- so both map
 * to the same operation and the START is issued once, lazily. */
/* HOW MANY TIMES THE SPEAKER RAN DRY -- the same count the AC'97 driver keeps,
 * arrived at differently because the two devices report differently.
 *
 * AC'97 latches "reached the end of the list and halted", which is the
 * hardware saying it. virtio-snd has no such bit, so the equivalent is
 * inferred from the one fact that means the same thing: at the moment new
 * audio arrives, was there anything still in flight? Nothing in flight while
 * running means the device finished everything it had and waited -- a hole,
 * and the writer is only now coming back.
 *
 * It is an inference, and the AC'97 one is not; that difference is real and is
 * why this comment says so rather than presenting the two as the same
 * measurement. Both answer the same question, and neither is a guess about
 * timing. Zeroed when a stream starts. */
static uint64_t g_underruns;

uint64_t ac97_underruns(void) { return g_underruns; }

static void submit_through(int last) {
    if (!g_up || last < 0 || last >= NBUFS)
        return;

    if (g_running) {
        reap();
        if (g_completed >= g_submitted)
            g_underruns++;      /* nothing was left playing; this is the hole */
    }

    if (!g_running) {
        if (!pcm_simple(VIRTIO_SND_R_PCM_START)) {
            kprintf("virtio-snd: PCM_START refused\n");
            return;
        }
        g_running = true;
    }

    while (g_next_submit != ((last + 1) % NBUFS)) {
        int i = g_next_submit;
        if (g_frames[i] == 0)
            break;

        /* Three descriptors: the stream header (device reads), the PCM data
         * (device reads), the status (device writes). */
        uint16_t d = (uint16_t)((g_submitted * 3) % g_tqsize);
        uint16_t d0 = d, d1 = (uint16_t)((d + 1) % g_tqsize),
                 d2 = (uint16_t)((d + 2) % g_tqsize);

        g_xfer[i].stream_id = 0;
        tdesc[d0].addr = dma(&g_xfer[i]); tdesc[d0].len = sizeof g_xfer[i];
        tdesc[d0].flags = VRING_DESC_F_NEXT; tdesc[d0].next = d1;
        tdesc[d1].addr = dma(g_buf[i]);
        tdesc[d1].len  = g_frames[i] * CHANNELS * sizeof(int16_t);
        tdesc[d1].flags = VRING_DESC_F_NEXT; tdesc[d1].next = d2;
        tdesc[d2].addr = dma(&g_stat[i]); tdesc[d2].len = sizeof g_stat[i];
        tdesc[d2].flags = VRING_DESC_F_WRITE; tdesc[d2].next = 0;

        uint16_t slot = tavail.idx % g_tqsize;
        tavail.ring[slot] = d0;
        __sync_synchronize();
        tavail.idx++;
        __sync_synchronize();

        g_submitted++;
        g_next_submit = (g_next_submit + 1) % NBUFS;
    }

    virtio_pci_notify(&g_vd, g_tnotify, VQ_TX);
}

void ac97_play(int last)     { submit_through(last); }
void ac97_set_last(int last) { submit_through(last); }

bool ac97_done(int last) {
    (void)last;
    if (!g_up) return true;
    reap();
    return g_completed >= g_submitted;
}

void ac97_stop(void) {
    if (!g_up || !g_running) return;
    pcm_simple(VIRTIO_SND_R_PCM_STOP);
    pcm_simple(VIRTIO_SND_R_PCM_RELEASE);
    g_running = false;

    /* Start the next stream from a clean ring rather than from wherever the
     * last one stopped: the indices are ours, and carrying them across a
     * stop/start is how a fresh sound inherits a stale buffer. */
    g_next_submit = 0;
    g_submitted = g_completed = 0;
    g_tlast_used = tused.idx;
    g_underruns = 0;              /* this sound's count, not the last one's */
    memset(g_frames, 0, sizeof g_frames);
}

void ac97_init(void) {
    const struct pci_device *pci = virtio_pci_find(VIRTIO_SND_DEVID, 0);
    if (!pci) {
        kprintf("virtio-snd: no device\n");
        return;
    }

    if (!virtio_pci_attach(&g_vd, pci, "virtio-snd", 0, 0))
        return;

    memset(cdesc, 0, sizeof cdesc); memset((void *)&cavail, 0, sizeof cavail);
    memset((void *)&cused, 0, sizeof cused);
    memset(tdesc, 0, sizeof tdesc); memset((void *)&tavail, 0, sizeof tavail);
    memset((void *)&tused, 0, sizeof tused);

    /* Polled, so the device must not raise its INTx line -- the same trap
     * virtio-blk documents: a level-triggered interrupt nothing acknowledges
     * wedges the machine the moment interrupts are on. */
    cavail.flags = VRING_AVAIL_F_NO_INTERRUPT;
    tavail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    g_cqsize = virtio_pci_setup_queue(&g_vd, VQ_CONTROL, CQ_SIZE, cdesc,
                                      &cavail, &cused, &g_cnotify);
    g_tqsize = virtio_pci_setup_queue(&g_vd, VQ_TX, TQ_SIZE, tdesc,
                                      &tavail, &tused, &g_tnotify);
    if (!g_cqsize || !g_tqsize) {
        kprintf("virtio-snd: control or tx queue missing\n");
        return;
    }
    virtio_pci_driver_ok(&g_vd);

    /* Describe the stream, then prepare it. period_bytes is one of OUR
     * buffers: it is the granularity the device reports progress at, so
     * matching it to the ring's buffer size is what makes a completion mean
     * "one buffer played". */
    struct pcm_set_params p;
    memset(&p, 0, sizeof p);
    p.hdr          = VIRTIO_SND_R_PCM_SET_PARAMS;
    p.stream_id    = 0;
    p.buffer_bytes = BUF_BYTES * 4;
    p.period_bytes = BUF_BYTES;
    p.channels     = (uint8_t)CHANNELS;
    p.format       = VIRTIO_SND_PCM_FMT_S16;
    p.rate         = VIRTIO_SND_PCM_RATE_44100;

    if (!ctl(&p, sizeof p)) {
        kprintf("virtio-snd: SET_PARAMS refused (S16 %d Hz, %d ch)\n",
                (int)RATE_HZ, (int)CHANNELS);
        return;
    }
    if (!pcm_simple(VIRTIO_SND_R_PCM_PREPARE)) {
        kprintf("virtio-snd: PCM_PREPARE refused\n");
        return;
    }

    g_up = true;
    kprintf("virtio-snd: stream 0 ready, S16 %d Hz %d ch, %d frames/buffer\n",
            (int)RATE_HZ, (int)CHANNELS, (int)FRAMES_PER_BUF);
}
