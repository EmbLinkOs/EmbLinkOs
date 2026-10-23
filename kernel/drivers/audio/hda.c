/* kernel/drivers/audio/hda.c -- Intel High Definition Audio.
 *
 * The sound card real machines actually have. AC'97 left the market around
 * 2008; every x86 laptop and desktop since carries an HDA controller (Intel
 * calls it Azalia, AMD and NVIDIA ship register-compatible ones), and until
 * this file existed a machine booting EmbLinkOS had no sound at all.
 *
 * ac97.c's header says HDA is "three or four times the work" and that was not
 * an excuse -- it is the measurement. Three things here have no AC'97
 * equivalent at all, and each is a way to produce perfect silence:
 *
 *   1. THE CODEC IS A SEPARATE CHIP ON A SERIAL LINK. The controller does not
 *      make sound; it DMAs samples onto a link where one or more codecs live,
 *      and it is reached only by posting verbs through a command ring (CORB)
 *      and reading answers out of a response ring (RIRB). Nothing about the
 *      audio path is a register write.
 *
 *   2. THE CODEC IS A GRAPH, not a device. Converters, mixers, selectors and
 *      pin complexes, wired differently on every machine, and the driver has
 *      to WALK it to find a route from a DAC to a jack that has something
 *      plugged into it. There is no fixed answer to "where does sound come
 *      out" -- that is what a pin's configuration default is for, and it is
 *      written by the board's firmware.
 *
 *   3. THE DMA ENGINE IS CYCLIC. AC'97 walks descriptors 0..LVI and HALTS;
 *      HDA walks a buffer of fixed total length (CBL) round and round forever
 *      and has no "stop here" mark. CBL cannot be rewritten while the engine
 *      runs. So the stream contract's `set_last`/`done` are RECONSTRUCTED
 *      below from the link position, and an underrun is silence played rather
 *      than a halt latched.
 *
 * WHAT THAT LAST ONE COSTS, said plainly rather than buried: a descriptor here
 * is a fixed page -- 1024 stereo frames, 21 ms at 48 kHz -- because the total
 * has to stay constant while the engine runs. AC'97 gained variable-length
 * descriptors and with them a sub-descriptor latency floor; this cannot have
 * them. audio_set_latency() below about 21 ms is therefore granted in name
 * only on this device. That is a property of HDA's engine, not of this
 * driver, and it is the same choice ALSA makes (a period size fixed for the
 * life of the stream). Recorded in docs/TODO.md.
 *
 * Fixed at 48 kHz, 16-bit, stereo -- the same format AC'97 is fixed at, so
 * every caller above this file sees one device and not two.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/pci.h"
#include "drivers/audio/pcm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* ---- controller registers (BAR0, MMIO) ---------------------------------- */
#define HDA_GCAP        0x00   /* 16: how many streams this controller has  */
#define HDA_GCTL        0x08   /* 32: bit 0 CRST -- 0 is reset, 1 is run    */
#define HDA_WAKEEN      0x0C
#define HDA_STATESTS    0x0E   /* 16: one bit per codec that answered       */
#define HDA_INTCTL      0x20
#define HDA_INTSTS      0x24
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48   /* 16 */
#define HDA_CORBRP      0x4A   /* 16 */
#define HDA_CORBCTL     0x4C   /* 8  */
#define HDA_CORBSIZE    0x4E   /* 8  */
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP      0x58   /* 16 */
#define HDA_RINTCNT     0x5A   /* 16 */
#define HDA_RIRBCTL     0x5C   /* 8  */
#define HDA_RIRBSTS     0x5D   /* 8  */
#define HDA_RIRBSIZE    0x5E   /* 8  */

#define GCTL_CRST       (1u << 0)
#define CORBCTL_RUN     (1u << 1)
#define CORBRP_RST      (1u << 15)
#define RIRBCTL_RINTCTL (1u << 0)
#define RIRBCTL_DMAEN   (1u << 1)
#define RIRBSTS_INTFL   (1u << 0)
#define RIRBSTS_OVERRUN (1u << 2)
#define RIRBWP_RST      (1u << 15)

/* ---- one stream descriptor ---------------------------------------------- */
#define SD_CTL          0x00   /* 24 bits, read as 32 with SDSTS on top     */
#define SD_STS          0x03   /* 8                                          */
#define SD_LPIB         0x04   /* 32: byte position inside the cyclic buffer */
#define SD_CBL          0x08   /* 32: cyclic buffer length, in bytes         */
#define SD_LVI          0x0C   /* 16: last valid BDL index                   */
#define SD_FIFOS        0x10   /* 16                                         */
#define SD_FMT          0x12   /* 16                                         */
#define SD_BDPL         0x18   /* 32                                         */
#define SD_BDPU         0x1C   /* 32                                         */

#define SDCTL_SRST      (1u << 0)
#define SDCTL_RUN       (1u << 1)
#define SDCTL_IOCE      (1u << 2)
#define SDCTL_STRM_SH   20     /* stream tag lives in bits 23:20            */

#define SDSTS_BCIS      (1u << 2)
#define SDSTS_FIFOE     (1u << 3)
#define SDSTS_DESE      (1u << 4)

/* 48 kHz, 16-bit, two channels. base=0 (48k), mult=1, div=1, bits=001,
 * chan=1 (it is channels-MINUS-ONE, which is the field most often shipped
 * one too high). */
#define HDA_FORMAT      0x0011
#define HDA_RATE        48000
#define HDA_STREAM_TAG  1

/* ---- verbs --------------------------------------------------------------
 * A command is (codec << 28) | (node << 20) | payload, where the 20-bit
 * payload is either a 12-bit verb with 8 bits of data or a 4-bit verb with
 * 16. Two encodings, one field -- which is why they get their own macros
 * instead of being written out at each call site. */
#define V12(v, d)   ((((uint32_t)(v)) << 8)  | ((uint32_t)(d) & 0xFF))
#define V4(v, d)    ((((uint32_t)(v)) << 16) | ((uint32_t)(d) & 0xFFFF))

#define GET_PARAM(p)            V12(0xF00, (p))
#define GET_CONN_ENTRY(o)       V12(0xF02, (o))
#define GET_CONFIG_DEFAULT      V12(0xF1C, 0)
#define SET_CONN_SELECT(i)      V12(0x701, (i))
#define SET_POWER_STATE(s)      V12(0x705, (s))
#define SET_STREAM_CHANNEL(v)   V12(0x706, (v))
#define SET_PIN_CTL(v)          V12(0x707, (v))
#define SET_EAPD_BTL(v)         V12(0x70C, (v))
#define SET_FORMAT(v)           V4 (0x2,   (v))
#define SET_AMP(v)              V4 (0x3,   (v))

#define PAR_VENDOR_ID       0x00
#define PAR_SUBNODE_COUNT   0x04
#define PAR_FUNCTION_TYPE   0x05
#define PAR_WIDGET_CAP      0x09
#define PAR_PIN_CAP         0x0C
#define PAR_CONN_LIST_LEN   0x0E
#define PAR_OUT_AMP_CAP     0x12

#define FUNC_TYPE_AUDIO     0x01

#define WIDGET_TYPE(cap)    (((cap) >> 20) & 0xF)
#define WID_DAC             0x0
#define WID_ADC             0x1
#define WID_MIXER           0x2
#define WID_SELECTOR        0x3
#define WID_PIN             0x4

#define WCAP_OUT_AMP        (1u << 2)
#define WCAP_IN_AMP         (1u << 1)
#define WCAP_CONN_LIST      (1u << 8)
#define WCAP_DIGITAL        (1u << 9)
#define WCAP_POWER          (1u << 10)

#define PINCAP_OUT          (1u << 4)
#define PINCTL_OUT_EN       (1u << 6)
#define PINCTL_HP_EN        (1u << 7)

/* Configuration default: the board's firmware describing the jack. */
#define CFG_CONNECTIVITY(c) (((c) >> 30) & 0x3)
#define CFG_DEVICE(c)       (((c) >> 20) & 0xF)
#define CONN_NONE           0x1     /* nothing is wired to this pin at all  */
#define DEV_LINE_OUT        0x0
#define DEV_SPEAKER         0x1
#define DEV_HP_OUT          0x2

/* ---- the ring, and why it is shaped like AC'97's ------------------------ */
#define HDA_BUFS        PCM_DESCRIPTORS          /* 32                       */
#define HDA_BUF_BYTES   4096                     /* one page per descriptor  */
#define HDA_BUF_FRAMES  (HDA_BUF_BYTES / 4)      /* stereo S16 -- 1024       */
#define HDA_CBL         ((uint32_t)HDA_BUFS * HDA_BUF_BYTES)

/* A buffer descriptor: 64-bit physical address, byte length, and a flag word
 * whose only bit anybody uses is "interrupt on completion". */
struct hda_bdl {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} __attribute__((packed));

#define CORB_ENTRIES 256
#define RIRB_ENTRIES 256

static struct {
    bool     present;
    volatile uint8_t *mmio;
    uint32_t sd;                       /* offset of output stream 0          */

    uint32_t *corb;   uint64_t corb_phys;   uint16_t corb_wp;
    uint64_t *rirb;   uint64_t rirb_phys;   uint16_t rirb_rp;

    struct hda_bdl *bdl;  uint64_t bdl_phys;
    int16_t *buf[HDA_BUFS];
    uint64_t buf_phys[HDA_BUFS];

    uint32_t codec;                    /* the codec address that answered    */
    uint32_t dac, pin;

    /* THE RECONSTRUCTION. LPIB is a byte offset inside a buffer that wraps,
     * so on its own it answers nothing; `played` is it unwrapped, and it is
     * what every question above this file is really asking. */
    uint32_t lpib;
    uint64_t played;                   /* bytes the link has taken, monotonic */
    uint64_t fills;                    /* descriptors filled since play()     */
    uint64_t seq[HDA_BUFS];            /* which fill each descriptor holds    */
    uint64_t last_seq;                 /* the fill the writer last declared   */
    bool     running;
    uint64_t underruns;
} g;

static inline uint8_t  r8 (uint32_t o)             { return *(volatile uint8_t  *)(g.mmio + o); }
static inline uint16_t r16(uint32_t o)             { return *(volatile uint16_t *)(g.mmio + o); }
static inline uint32_t r32(uint32_t o)             { return *(volatile uint32_t *)(g.mmio + o); }
static inline void     w8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g.mmio + o) = v; }
static inline void     w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g.mmio + o) = v; }
static inline void     w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g.mmio + o) = v; }

/* Bring-up only. There is no scheduler to yield to this early, and the codec
 * link needs real microseconds after a reset before it will answer. */
static void hda_delay(void) { for (volatile int i = 0; i < 200000; i++) { } }

/* ---- the command ring ---------------------------------------------------
 *
 * One verb at a time, and the answer waited for. A driver that pipelines
 * verbs has to match responses to commands by position, and every one of the
 * calls below needs its answer before it can decide what to ask next -- so
 * the pipeline would be depth one anyway. */
#define HDA_NO_ANSWER 0xFFFFFFFFu

static uint32_t codec_cmd(uint32_t cad, uint32_t nid, uint32_t payload)
{
    uint32_t verb = (cad << 28) | (nid << 20) | (payload & 0xFFFFF);

    g.corb_wp = (uint16_t)((g.corb_wp + 1) % CORB_ENTRIES);
    g.corb[g.corb_wp] = verb;
    __sync_synchronize();
    w16(HDA_CORBWP, g.corb_wp);

    /* Wait for the response ring's write pointer to move past ours. Bounded:
     * a codec that does not answer must leave a message, not a hung boot. */
    for (int spin = 0; spin < 2000000; spin++) {
        uint16_t wp = (uint16_t)(r16(HDA_RIRBWP) & 0xFF);
        if (wp != g.rirb_rp) {
            g.rirb_rp = (uint16_t)((g.rirb_rp + 1) % RIRB_ENTRIES);
            __sync_synchronize();
            uint64_t resp = g.rirb[g.rirb_rp];
            /* ACKNOWLEDGE THE RESPONSE, and it is not bookkeeping. The
             * controller counts responses against RINTCNT and stops FETCHING
             * COMMANDS once it reaches it -- this write is what resets the
             * count. Skip it and exactly one verb ever executes, which reads
             * as "the codec does not answer" and is the controller never
             * having asked it. */
            w8(HDA_RIRBSTS, RIRBSTS_INTFL | RIRBSTS_OVERRUN);
            return (uint32_t)resp;
        }
        __asm__ volatile("" ::: "memory");
    }
    return HDA_NO_ANSWER;
}

/* ---- walking the codec graph -------------------------------------------
 *
 * From a pin the board says is wired to something, find the converter that
 * can feed it, unmuting and selecting along the way. Depth-limited because a
 * codec graph is a graph: a mixer can list a widget that lists it back, and a
 * driver that follows that faithfully never finishes booting. */
static void unmute_output(uint32_t nid, uint32_t cap)
{
    if (!(cap & WCAP_OUT_AMP)) return;
    uint32_t amp = codec_cmd(g.codec, nid, GET_PARAM(PAR_OUT_AMP_CAP));
    uint32_t steps = (amp == HDA_NO_ANSWER) ? 0x7F : ((amp >> 8) & 0x7F);
    /* bit 15 output amp, 13 left, 12 right, bit 7 mute (clear), gain in 6:0.
     * An HDA codec powers up with its output amps MUTED, exactly as AC'97
     * does -- a driver that never writes this is correct and inaudible. */
    codec_cmd(g.codec, nid, SET_AMP(0xB000 | (steps & 0x7F)));
}

static void unmute_input(uint32_t nid, uint32_t cap, uint32_t index)
{
    if (!(cap & WCAP_IN_AMP)) return;
    /* bit 14 input amp, 13 left, 12 right, index in 11:8, mute clear, full
     * gain. A mixer that is unmuted on the wrong INDEX passes silence from
     * the input nobody is using. */
    codec_cmd(g.codec, nid, SET_AMP(0x7000 | ((index & 0xF) << 8) | 0x00));
}

/* Read connection `i` of `nid`, or -1. Entries are packed four to a response
 * in the short form and two in the long one, and the form is a bit in the
 * length parameter rather than anything visible in the answer. */
static int conn_entry(uint32_t nid, uint32_t len_param, uint32_t i)
{
    bool longform = (len_param & 0x80) != 0;
    uint32_t per = longform ? 2u : 4u;
    uint32_t resp = codec_cmd(g.codec, nid, GET_CONN_ENTRY((i / per) * per));
    if (resp == HDA_NO_ANSWER) return -1;
    if (longform) return (int)((resp >> ((i % per) * 16)) & 0x7FFF);
    return (int)((resp >> ((i % per) * 8)) & 0x7F);
}

static int find_dac(uint32_t nid, int depth)
{
    if (depth > 4) return -1;

    uint32_t cap = codec_cmd(g.codec, nid, GET_PARAM(PAR_WIDGET_CAP));
    if (cap == HDA_NO_ANSWER) return -1;
    if (WIDGET_TYPE(cap) == WID_DAC && !(cap & WCAP_DIGITAL)) return (int)nid;
    if (!(cap & WCAP_CONN_LIST)) return -1;

    uint32_t lp = codec_cmd(g.codec, nid, GET_PARAM(PAR_CONN_LIST_LEN));
    if (lp == HDA_NO_ANSWER) return -1;
    uint32_t n = lp & 0x7F;

    for (uint32_t i = 0; i < n; i++) {
        int child = conn_entry(nid, lp, i);
        if (child <= 0) continue;
        int dac = find_dac((uint32_t)child, depth + 1);
        if (dac < 0) continue;

        /* This path works. Commit to it on the way back out: a selector has
         * to be POINTED at the branch, and a mixer's input for that branch
         * has to be unmuted, or the route exists on paper only. */
        if (WIDGET_TYPE(cap) == WID_SELECTOR || WIDGET_TYPE(cap) == WID_PIN) {
            if (n > 1) codec_cmd(g.codec, nid, SET_CONN_SELECT(i));
        }
        unmute_input(nid, cap, i);
        unmute_output(nid, cap);
        if (cap & WCAP_POWER) codec_cmd(g.codec, nid, SET_POWER_STATE(0));
        return dac;
    }
    return -1;
}

/* How much we want a given pin, higher is better. The board's own firmware
 * fills in the configuration default; this reads it rather than guessing from
 * node numbers, which is the difference between working on one machine and
 * working on the next one. */
static int pin_rank(uint32_t cfg)
{
    if (CFG_CONNECTIVITY(cfg) == CONN_NONE) return -1;   /* not wired at all */
    switch (CFG_DEVICE(cfg)) {
    case DEV_SPEAKER:  return 3;
    case DEV_LINE_OUT: return 2;
    case DEV_HP_OUT:   return 1;
    default:           return 0;
    }
}

static bool codec_setup(uint32_t cad)
{
    uint32_t vid = codec_cmd(cad, 0, GET_PARAM(PAR_VENDOR_ID));
    if (vid == HDA_NO_ANSWER || vid == 0) {
        kprintf("hda: codec %u is on the link but did not answer\n", (unsigned)cad);
        return false;
    }
    g.codec = cad;

    uint32_t sub = codec_cmd(cad, 0, GET_PARAM(PAR_SUBNODE_COUNT));
    if (sub == HDA_NO_ANSWER) return false;
    uint32_t fg_start = (sub >> 16) & 0xFF, fg_count = sub & 0xFF;

    for (uint32_t f = 0; f < fg_count; f++) {
        uint32_t afg = fg_start + f;
        uint32_t type = codec_cmd(cad, afg, GET_PARAM(PAR_FUNCTION_TYPE));
        if (type == HDA_NO_ANSWER || (type & 0xFF) != FUNC_TYPE_AUDIO) continue;

        codec_cmd(cad, afg, SET_POWER_STATE(0));
        hda_delay();

        uint32_t ws = codec_cmd(cad, afg, GET_PARAM(PAR_SUBNODE_COUNT));
        if (ws == HDA_NO_ANSWER) continue;
        uint32_t w_start = (ws >> 16) & 0xFF, w_count = ws & 0xFF;

        int best_pin = -1, best_rank = -1, pins = 0;
        for (uint32_t i = 0; i < w_count; i++) {
            uint32_t nid = w_start + i;
            uint32_t cap = codec_cmd(cad, nid, GET_PARAM(PAR_WIDGET_CAP));
            if (cap == HDA_NO_ANSWER) continue;
            if (WIDGET_TYPE(cap) != WID_PIN || (cap & WCAP_DIGITAL)) continue;

            uint32_t pc = codec_cmd(cad, nid, GET_PARAM(PAR_PIN_CAP));
            if (pc == HDA_NO_ANSWER || !(pc & PINCAP_OUT)) continue;

            pins++;
            uint32_t cfg = codec_cmd(cad, nid, GET_CONFIG_DEFAULT);
            int rank = (cfg == HDA_NO_ANSWER) ? 0 : pin_rank(cfg);
            if (rank > best_rank) { best_rank = rank; best_pin = (int)nid; }
        }
        if (best_pin < 0) {
            kprintf("hda: codec %u function %u has %d output pin(s), none usable\n",
                    (unsigned)cad, (unsigned)afg, pins);
            continue;
        }

        int dac = find_dac((uint32_t)best_pin, 0);
        if (dac < 0) {
            kprintf("hda: pin %d is wired to nothing this driver can drive\n", best_pin);
            continue;
        }

        g.pin = (uint32_t)best_pin;
        g.dac = (uint32_t)dac;

        /* Turn the jack on. OUT_ENABLE is the one that matters; HP_ENABLE
         * drives the headphone amplifier and is harmless on a pin that has
         * none. EAPD is the external amplifier on laptops -- clearing it is
         * how a correct driver ends up audible on a desktop and silent on a
         * notebook. */
        codec_cmd(cad, g.pin, SET_PIN_CTL(PINCTL_OUT_EN | PINCTL_HP_EN));
        codec_cmd(cad, g.pin, SET_EAPD_BTL(0x02));
        codec_cmd(cad, g.pin, SET_POWER_STATE(0));

        uint32_t dcap = codec_cmd(cad, g.dac, GET_PARAM(PAR_WIDGET_CAP));
        if (dcap != HDA_NO_ANSWER) unmute_output(g.dac, dcap);
        codec_cmd(cad, g.dac, SET_POWER_STATE(0));
        codec_cmd(cad, g.dac, SET_FORMAT(HDA_FORMAT));
        codec_cmd(cad, g.dac, SET_STREAM_CHANNEL((HDA_STREAM_TAG << 4) | 0));

        kprintf("hda: codec %u vendor 0x%x -- dac node %u -> pin node %u\n",
                (unsigned)cad, (unsigned)vid, (unsigned)g.dac, (unsigned)g.pin);
        return true;
    }
    return false;
}

/* ---- the stream engine -------------------------------------------------- */

/* Unwrap LPIB. Called by everything that asks a question about position, and
 * often enough that a full lap cannot pass between two calls -- which at
 * 170 ms a lap means a writer that has stopped writing, and that shows up as
 * an underrun rather than as a lost wrap. */
static void hda_sync(void)
{
    if (!g.running) return;
    uint32_t lpib = r32(g.sd + SD_LPIB);
    if (lpib >= HDA_CBL) lpib %= HDA_CBL;
    g.played += (lpib >= g.lpib) ? (lpib - g.lpib) : (HDA_CBL - g.lpib + lpib);
    g.lpib = lpib;

}

static bool hda_probe(const struct pci_device **out)
{
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        /* BY CLASS, not by a device-id table. Intel alone has shipped dozens
         * of HDA controller ids and AMD, NVIDIA and VIA ship their own; they
         * are all register-compatible and all declare class 04 subclass 03,
         * which is the only identification that keeps working on a machine
         * built after this was written. */
        if (d && d->class_code == 0x04 && d->subclass == 0x03) { *out = d; return true; }
    }
    return false;
}

static bool rings_init(void)
{
    uint64_t p = pmm_alloc_page();
    if (!p) return false;
    g.corb_phys = p;
    g.corb = (uint32_t *)P2V(p);
    memset(g.corb, 0, PAGE_SIZE);

    p = pmm_alloc_page();
    if (!p) return false;
    g.rirb_phys = p;
    g.rirb = (uint64_t *)P2V(p);
    memset(g.rirb, 0, PAGE_SIZE);

    /* Stop both engines before moving their bases. A controller that is still
     * fetching from the old address will happily execute whatever is there. */
    w8(HDA_CORBCTL, 0);
    w8(HDA_RIRBCTL, 0);

    w8(HDA_CORBSIZE, 0x02);                 /* 256 entries                   */
    w32(HDA_CORBLBASE, (uint32_t)g.corb_phys);
    w32(HDA_CORBUBASE, (uint32_t)(g.corb_phys >> 32));

    /* Reset the read pointer, and CONFIRM IT. The bit is write-1, hardware
     * clears it, and a controller that never acknowledges is one whose ring
     * would start at an unknown index -- every verb off by one forever. */
    w16(HDA_CORBRP, CORBRP_RST);
    for (int i = 0; i < 1000 && !(r16(HDA_CORBRP) & CORBRP_RST); i++) hda_delay();
    w16(HDA_CORBRP, 0);
    for (int i = 0; i < 1000 && (r16(HDA_CORBRP) & CORBRP_RST); i++) hda_delay();
    w16(HDA_CORBWP, 0);
    g.corb_wp = 0;

    w8(HDA_RIRBSIZE, 0x02);
    w32(HDA_RIRBLBASE, (uint32_t)g.rirb_phys);
    w32(HDA_RIRBUBASE, (uint32_t)(g.rirb_phys >> 32));
    w16(HDA_RIRBWP, RIRBWP_RST);
    w16(HDA_RINTCNT, 1);
    g.rirb_rp = 0;

    w8(HDA_CORBCTL, CORBCTL_RUN);
    /* RINTCTL is on even though nothing here takes an interrupt: it is what
     * makes the controller RAISE the response flag, and the flag is what
     * codec_cmd() clears to let the next command through. INTCTL's global
     * enable is off, so no interrupt is delivered to the CPU -- the flag is
     * used as a flag, which is all a polled driver needs from it. */
    w8(HDA_RIRBCTL, RIRBCTL_DMAEN | RIRBCTL_RINTCTL);
    return true;
}

static bool hda_init(void)
{
    const struct pci_device *d = NULL;
    if (!hda_probe(&d)) return false;

    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("hda: BAR0 is not memory -- refusing to guess\n");
        return false;
    }
    g.mmio = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                         bar.size ? bar.size : 0x4000);
    if (!g.mmio) { kprintf("hda: could not map BAR0\n"); return false; }

    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* Intel's controllers default to a traffic class the chipset may not
     * snoop, and the symptom is audio that plays from stale cache lines --
     * intermittent, machine-dependent noise that looks like a driver bug.
     * Linux clears the same register for the same reason. */
    if (d->vendor_id == 0x8086)
        pci_write32(d->bus, d->device, d->function, 0x44,
                    pci_read32(d->bus, d->device, d->function, 0x44) & ~0xFFu);

    /* Out of reset. CRST is LOW-true: the controller is held in reset while
     * the bit is 0, and it must be taken there deliberately first, because
     * firmware may have left it running. */
    w32(HDA_GCTL, r32(HDA_GCTL) & ~GCTL_CRST);
    for (int i = 0; i < 1000 && (r32(HDA_GCTL) & GCTL_CRST); i++) hda_delay();
    w32(HDA_GCTL, r32(HDA_GCTL) | GCTL_CRST);
    for (int i = 0; i < 1000 && !(r32(HDA_GCTL) & GCTL_CRST); i++) hda_delay();
    if (!(r32(HDA_GCTL) & GCTL_CRST)) {
        kprintf("hda: controller never came out of reset\n");
        return false;
    }
    /* The codecs need real time on the link before STATESTS means anything.
     * Reading it too early finds no codecs on hardware that has several. */
    hda_delay(); hda_delay(); hda_delay();

    uint16_t gcap = r16(HDA_GCAP);
    uint32_t iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (oss == 0) { kprintf("hda: controller has no output stream\n"); return false; }
    /* Stream descriptors are laid out inputs first, then outputs, then
     * bidirectional -- so where the first OUTPUT one lives depends on how many
     * inputs this particular controller has. */
    g.sd = 0x80 + iss * 0x20;

    w32(HDA_INTCTL, 0);                 /* polled, like every driver here    */
    w16(HDA_WAKEEN, 0);

    if (!rings_init()) { kprintf("hda: no memory for the command rings\n"); return false; }

    uint16_t statests = r16(HDA_STATESTS);
    w16(HDA_STATESTS, statests);        /* write-1-to-clear the latch        */
    if (statests == 0) { kprintf("hda: no codec answered\n"); return false; }

    bool found = false;
    for (uint32_t cad = 0; cad < 15 && !found; cad++)
        if (statests & (1u << cad)) found = codec_setup(cad);
    if (!found) { kprintf("hda: no codec with an output path\n"); return false; }

    /* The descriptor list and one page of PCM behind each entry. The list is
     * 128-byte aligned by being page-aligned, which the spec requires and a
     * page allocator gives for free. */
    uint64_t bp = pmm_alloc_page();
    if (!bp) { kprintf("hda: no page for the descriptor list\n"); return false; }
    g.bdl_phys = bp;
    g.bdl = (struct hda_bdl *)P2V(bp);
    memset(g.bdl, 0, PAGE_SIZE);

    for (int i = 0; i < HDA_BUFS; i++) {
        uint64_t q = pmm_alloc_page();
        if (!q) { kprintf("hda: only %d buffers\n", i); return false; }
        g.buf_phys[i] = q;
        g.buf[i] = (int16_t *)P2V(q);
        memset(g.buf[i], 0, PAGE_SIZE);
        g.bdl[i].addr  = q;
        g.bdl[i].len   = HDA_BUF_BYTES;
        g.bdl[i].flags = 0;
    }

    g.present = true;
    kprintf("hda: %u out / %u in streams, stream regs at 0x%x, %u Hz, "
            "%u frames/buffer\n",
            (unsigned)oss, (unsigned)iss, (unsigned)g.sd,
            (unsigned)HDA_RATE, (unsigned)HDA_BUF_FRAMES);
    return true;
}

static uint32_t hda_sample_rate(void)       { return g.present ? HDA_RATE : 0; }
static uint32_t hda_frames_per_buffer(void) { return HDA_BUF_FRAMES; }

/* EVERY DESCRIPTOR PLAYS A FULL PAGE, whatever was put in it -- that is what
 * a cyclic engine with a fixed CBL means, and it is why this op exists. The
 * stream layer uses it for the playback position, which would otherwise lose
 * the padding on every short write and drift. */
static uint32_t hda_desc_frames(int i) { (void)i; return HDA_BUF_FRAMES; }

static uint32_t hda_fill(int i, const int16_t *frames, uint32_t nframes)
{
    if (!g.present || i < 0 || i >= HDA_BUFS || !frames) return 0;

    uint32_t take = nframes < HDA_BUF_FRAMES ? nframes : HDA_BUF_FRAMES;
    if (take == 0) return 0;

    memcpy(g.buf[i], frames, (size_t)take * 4);
    if (take < HDA_BUF_FRAMES)
        memset(g.buf[i] + take * 2, 0, (size_t)(HDA_BUF_FRAMES - take) * 4);

    g.seq[i] = ++g.fills;

    /* SILENCE ONE STEP AHEAD. The engine never stops, so when the writer
     * falls behind it walks straight into the next descriptor and plays
     * whatever is there -- which, a lap later, is audio from 680 ms ago. A
     * repeat is far worse than a gap, and clearing the descriptor the engine
     * will reach next turns one into the other. Safe to clear here because
     * the stream layer never fills further ahead than this. */
    int nxt = (i + 1) % HDA_BUFS;
    memset(g.buf[nxt], 0, HDA_BUF_BYTES);
    g.seq[nxt] = 0;

    return take;
}

static uint8_t hda_civ(void)
{
    if (!g.present || !g.running) return 0;
    hda_sync();
    return (uint8_t)((g.lpib / HDA_BUF_BYTES) % HDA_BUFS);
}

/* "The valid range now reaches `last`."
 *
 * There is nothing to write: the engine is cyclic and already walking. What
 * this call is good for is DETECTING that it walked past the end of what it
 * had been given before the extension arrived -- which is an underrun, and on
 * this device it is silence played rather than AC'97's latched halt. Same
 * hole in the sound, same number, counted where the hardware shows it. */
static void hda_set_last(int last)
{
    if (!g.present || last < 0 || last >= HDA_BUFS) return;
    hda_sync();
    /* MORE AUDIO ARRIVING AFTER THE ENGINE PASSED THE END OF THE LAST LOT is
     * the underrun, and it is the only way this device has of showing one:
     * AC'97 halts and latches a bit, this walks into the silence beyond and
     * keeps going. Counting it HERE rather than in the position poll is what
     * separates it from simply reaching the end of a sound -- nothing follows
     * that, so nothing is counted, which is correct and was not the first
     * version's behaviour. */
    if (g.running && g.played > g.last_seq * (uint64_t)HDA_BUF_BYTES)
        g.underruns++;
    g.last_seq = g.seq[last];
}

static void hda_play(int last)
{
    if (!g.present || last < 0 || last >= HDA_BUFS) return;

    /* Stop and reset the stream engine before touching anything it reads.
     * SRST is also low-true and also has to be confirmed both ways -- a
     * controller half out of reset accepts register writes and ignores them. */
    w32(g.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (r32(g.sd + SD_CTL) & SDCTL_RUN); i++) hda_delay();
    w32(g.sd + SD_CTL, SDCTL_SRST);
    for (int i = 0; i < 1000 && !(r32(g.sd + SD_CTL) & SDCTL_SRST); i++) hda_delay();
    w32(g.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (r32(g.sd + SD_CTL) & SDCTL_SRST); i++) hda_delay();

    w8(g.sd + SD_STS, SDSTS_BCIS | SDSTS_FIFOE | SDSTS_DESE);

    w32(g.sd + SD_CBL, HDA_CBL);
    w16(g.sd + SD_LVI, (uint16_t)(HDA_BUFS - 1));
    w32(g.sd + SD_BDPL, (uint32_t)g.bdl_phys);
    w32(g.sd + SD_BDPU, (uint32_t)(g.bdl_phys >> 32));
    w16(g.sd + SD_FMT, HDA_FORMAT);

    /* The stream TAG is what ties this engine to the converter that was told
     * the same number during codec setup. Get it wrong and the DMA runs
     * perfectly into a link nothing is listening on. */
    w32(g.sd + SD_CTL, ((uint32_t)HDA_STREAM_TAG << SDCTL_STRM_SH));

    /* Descriptors 0..last hold this sound; the engine starts at 0 and byte 0,
     * so the fill sequence is exactly the index plus one. Recomputing it here
     * rather than trusting whatever fill() left makes a restart correct even
     * if the previous stream ended mid-ring. */
    g.fills = (uint64_t)last + 1;
    for (int i = 0; i < HDA_BUFS; i++) g.seq[i] = (i <= last) ? (uint64_t)(i + 1) : 0;
    g.last_seq = (uint64_t)last + 1;
    g.lpib = 0;
    g.played = 0;
    g.underruns = 0;          /* this sound's count, not the last one's */
    g.running = true;

    __sync_synchronize();
    w32(g.sd + SD_CTL, ((uint32_t)HDA_STREAM_TAG << SDCTL_STRM_SH) | SDCTL_RUN);
}

static bool hda_done(int last)
{
    if (!g.present || !g.running || last < 0 || last >= HDA_BUFS) return true;
    hda_sync();
    uint64_t end = g.seq[last] * (uint64_t)HDA_BUF_BYTES;
    return end == 0 || g.played >= end;
}

static void hda_stop(void)
{
    if (!g.present) return;
    w32(g.sd + SD_CTL, 0);
    for (int i = 0; i < 1000 && (r32(g.sd + SD_CTL) & SDCTL_RUN); i++) hda_delay();
    g.running = false;
    /* Leave the ring silent. The next sound fills from descriptor 0 and the
     * engine starts there, but anything it has not reached yet would be the
     * tail of this one. */
    for (int i = 0; i < HDA_BUFS; i++) {
        if (g.buf[i]) memset(g.buf[i], 0, HDA_BUF_BYTES);
        g.seq[i] = 0;
    }
    g.fills = 0;
    g.last_seq = 0;
}

static uint64_t hda_underruns(void) { return g.underruns; }

const struct pcm_driver hda_driver = {
    .name              = "hda",
    .init              = hda_init,
    .sample_rate       = hda_sample_rate,
    .frames_per_buffer = hda_frames_per_buffer,
    .fill              = hda_fill,
    .desc_frames       = hda_desc_frames,
    .civ               = hda_civ,
    .set_last          = hda_set_last,
    .play              = hda_play,
    .done              = hda_done,
    .stop              = hda_stop,
    .underruns         = hda_underruns,
};
