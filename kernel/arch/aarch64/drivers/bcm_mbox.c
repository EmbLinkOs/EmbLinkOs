#include "arch/aarch64/drivers/bcm_mbox.h"
#include "arch/aarch64/boot/fdt.h"
#include "drivers/video/framebuffer.h"
#include "mm/pmm.h"
#include "include/kprintf.h"

/* The VideoCore mailbox's property channel -- see bcm_mbox.h.
 *
 * The mailbox is a pair of FIFOs between the ARM and the VideoCore (the GPU,
 * which on a Pi is also the thing that booted us). A message is a buffer in
 * RAM; the ARM writes the buffer's BUS address, tagged with a channel number,
 * into the ARM->VC FIFO and waits for the same word to come back on VC->ARM.
 * The firmware has then rewritten the buffer in place with its answers.
 *
 * Register layout and the property tags: the raspberrypi/firmware wiki,
 * "Mailbox property interface". Mailbox 0 is VC->ARM (we read it), mailbox 1
 * is ARM->VC (we write it) -- each has its OWN status register, and polling
 * mailbox 0's FULL bit before writing mailbox 1 is a classic bug that works
 * until the day the FIFO is actually full. */

#define MBOX0_READ      0x00
#define MBOX0_STATUS    0x18
#define MBOX1_WRITE     0x20
#define MBOX1_STATUS    0x38
#define MBOX_FULL       (1u << 31)
#define MBOX_EMPTY      (1u << 30)
#define CH_PROPERTY     8

#define REQUEST         0x00000000u
#define RESPONSE_OK     0x80000000u

#define TAG_GET_PHYS_WH   0x00040003u
#define TAG_SET_PHYS_WH   0x00048003u
#define TAG_SET_VIRT_WH   0x00048004u
#define TAG_SET_VIRT_OFF  0x00048009u
#define TAG_SET_DEPTH     0x00048005u
#define TAG_SET_PIXORDER  0x00048006u
#define TAG_ALLOC_BUFFER  0x00040001u
#define TAG_GET_PITCH     0x00040008u
#define TAG_GET_CLOCK_RATE 0x00030002u
#define TAG_NOTIFY_XHCI_RESET 0x00030058u

/* PIXEL ORDER, as the firmware names it: 0 = BGR, 1 = RGB -- the order of the
 * bytes in memory, which is what FB_FORMAT_* describes too. We ASK for RGB and
 * then believe the answer, not the request: firmware releases have differed in
 * whether they honour it. */
#define PIXORDER_BGR    0
#define PIXORDER_RGB    1

/* The largest framebuffer asked for. A 4K television reports 3840x2160, and a
 * 32-bpp buffer that size is 33 MB out of a default GPU memory split of 76 MB
 * -- an allocation that fails on some firmware and wins nothing at boot. The
 * display still runs its native mode; the firmware scales. */
#define FB_MAX_W        1920
#define FB_MAX_H        1080

/* 16-byte aligned: the low 4 bits of the word written to the FIFO are the
 * channel, so the buffer address must have them clear. */
static volatile uint32_t msg[48] __attribute__((aligned(64)));

static volatile uint8_t *mbox;           /* the registers, through MMIO_BASE */
static fdt_node_t mbox_node;

static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t *)(mbox + off); }
static inline void w32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(mbox + off) = v; }

static uint64_t counter(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
static uint64_t counter_hz(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v ? v : 1;
}

/* The VideoCore is not coherent with the ARM's caches. Before it reads the
 * message, the ARM's dirty lines must reach RAM; after it writes, the ARM's
 * stale lines must go. `dc civac` does both (clean, then invalidate) to the
 * point of coherency, which on this SoC is DRAM. */
static void cache_clean_invalidate(volatile void *p, uint64_t len) {
    uint64_t a = (uint64_t)(uintptr_t)p & ~63ull, end = (uint64_t)(uintptr_t)p + len;
    for (; a < end; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

/* One round trip. False on a timeout (one second: the firmware answers in
 * microseconds, so a timeout is a mailbox that is not there) or on a message
 * the firmware did not accept as a whole. */
static bool mbox_call(void) {
    uint64_t pa = KV2P((uint64_t)(uintptr_t)msg), bus;
    if (!fdt_dma_translate(mbox_node, true, pa, &bus))
        bus = pa;                       /* no dma-ranges: the buses agree */
    if (bus > 0xFFFFFFFFull || (bus & 0xF))
        return false;

    cache_clean_invalidate(msg, sizeof msg);

    uint64_t deadline = counter() + counter_hz();
    while (r32(MBOX1_STATUS) & MBOX_FULL)
        if (counter() > deadline) return false;
    w32(MBOX1_WRITE, (uint32_t)bus | CH_PROPERTY);

    for (;;) {
        while (r32(MBOX0_STATUS) & MBOX_EMPTY)
            if (counter() > deadline) return false;
        uint32_t v = r32(MBOX0_READ);
        if ((v & 0xF) == CH_PROPERTY && (v & ~0xFu) == ((uint32_t)bus & ~0xFu))
            break;                      /* ours; anything else is another channel's */
    }

    cache_clean_invalidate(msg, sizeof msg);
    return msg[1] == RESPONSE_OK;
}

/* Message building: tag, value-buffer size, request code, values. Returns the
 * index of the tag's first value word, so the answer can be read back from it. */
static uint32_t n;
static void begin(void) { n = 2; }
static uint32_t tag(uint32_t id, uint32_t words, const uint32_t *v) {
    msg[n++] = id;
    msg[n++] = words * 4;
    msg[n++] = REQUEST;
    uint32_t at = n;
    for (uint32_t i = 0; i < words; i++)
        msg[n++] = v ? v[i] : 0;
    return at;
}
static void end(void) {
    msg[n++] = 0;                       /* the end tag */
    msg[0] = n * 4;
    msg[1] = REQUEST;
}

void bcm_fb_probe(struct boot_protocol *proto) {
    mbox_node = fdt_find_compatible("brcm,bcm2835-mbox");
    if (mbox_node == FDT_NONE)
        return;                          /* not a Pi: nothing to say */

    uint64_t phys = 0, size = 0;
    if (!fdt_reg(mbox_node, 0, &phys, &size)) {
        kprintf("mbox: the device tree's mailbox has no usable address\n");
        return;
    }
    /* boot.S maps only the Pi's peripheral GiB (0xC0000000..) into the MMIO
     * window this early, and pmm does not exist yet to map anything else. */
    if (phys < 0xC0000000ull || phys >= 0x100000000ull) {
        kprintf("mbox: %p is outside the boot MMIO window -- no framebuffer\n",
                (void *)(uintptr_t)phys);
        return;
    }
    mbox = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + phys);

    /* What is the display? The firmware has read the TV's EDID by now; with no
     * display attached it reports 0x0 (or, with hdmi_force_hotplug, a default). */
    begin();
    uint32_t wh = tag(TAG_GET_PHYS_WH, 2, 0);
    end();
    uint32_t w = 0, h = 0;
    if (mbox_call()) { w = msg[wh]; h = msg[wh + 1]; }
    kprintf("mbox: firmware reports display %ux%u\n", w, h);
    if (!w || !h) { w = 1280; h = 720; }        /* ask for something sane */
    if (w > FB_MAX_W || h > FB_MAX_H) { w = FB_MAX_W; h = FB_MAX_H; }

    uint32_t whv[2]  = { w, h };
    uint32_t zero[2] = { 0, 0 };
    uint32_t depth   = 32;
    uint32_t order   = PIXORDER_RGB;
    uint32_t align[2] = { 4096, 0 };

    begin();
    uint32_t i_phys  = tag(TAG_SET_PHYS_WH,  2, whv);
    (void)             tag(TAG_SET_VIRT_WH,  2, whv);
    (void)             tag(TAG_SET_VIRT_OFF, 2, zero);
    uint32_t i_depth = tag(TAG_SET_DEPTH,    1, &depth);
    uint32_t i_order = tag(TAG_SET_PIXORDER, 1, &order);
    uint32_t i_buf   = tag(TAG_ALLOC_BUFFER, 2, align);
    uint32_t i_pitch = tag(TAG_GET_PITCH,    1, 0);
    end();

    if (!mbox_call()) {
        kprintf("mbox: the firmware refused the framebuffer request -- no screen\n");
        return;
    }

    uint32_t fw = msg[i_phys], fh = msg[i_phys + 1];
    uint32_t bpp = msg[i_depth], pitch = msg[i_pitch];
    uint64_t fb_bus = msg[i_buf], fb_size = msg[i_buf + 1], fb;

    /* The buffer comes back as a VideoCore BUS address. Through the bus's
     * dma-ranges when it has them; else the firmware's documented habit of
     * setting the top two bits for the uncached alias. */
    if (!fdt_dma_translate(mbox_node, false, fb_bus, &fb))
        fb = fb_bus & 0x3FFFFFFFull;

    if (!fb || !fw || !fh || bpp != 32 || pitch < fw * 4 ||
        (uint64_t)pitch * fh > fb_size) {
        kprintf("mbox: unusable framebuffer: %ux%u %u bpp, pitch %u, %p + %u\n",
                fw, fh, bpp, pitch, (void *)(uintptr_t)fb, (uint32_t)fb_size);
        return;
    }

    proto->fb_addr   = fb;
    proto->fb_width  = fw;
    proto->fb_height = fh;
    proto->fb_pitch  = pitch;
    proto->fb_bpp    = 32;
    proto->fb_format = msg[i_order] == PIXORDER_BGR ? FB_FORMAT_BGR : FB_FORMAT_RGB;

    kprintf("mbox: framebuffer %ux%u, pitch %u, %s, at %p (bus %p)\n",
            fw, fh, pitch, proto->fb_format == FB_FORMAT_BGR ? "BGR" : "RGB",
            (void *)(uintptr_t)fb, (void *)(uintptr_t)fb_bus);
}

uint32_t bcm_mbox_clock_rate(uint32_t clock_id) {
    if (!mbox)
        return 0;                       /* bcm_fb_probe found no mailbox */
    uint32_t v[2] = { clock_id, 0 };
    begin();
    uint32_t at = tag(TAG_GET_CLOCK_RATE, 2, v);
    end();
    if (!mbox_call() || msg[at] != clock_id)
        return 0;
    return msg[at + 1];
}

bool bcm_mbox_notify_xhci_reset(uint32_t pci_dev_addr) {
    if (!mbox)
        return false;
    begin();
    uint32_t at = tag(TAG_NOTIFY_XHCI_RESET, 1, &pci_dev_addr);
    end();
    (void)at;
    return mbox_call();
}
