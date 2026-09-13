/* kernel/net/rtl8139.c -- Realtek RTL8139, the second vendor on this machine.
 *
 * WHY A SECOND VENDOR AT ALL. docs/PILLARS.md scopes the "real network card"
 * pillar as Intel *and* Realtek, because between them they are most wired
 * machines. One driver proves a card works; two prove the DRIVER TABLE works --
 * that `struct net_driver` is a real seam and not a shape drawn around the one
 * implementation that existed.
 *
 * WHY THE 8139 AND NOT THE 8169. QEMU emulates `rtl8139` and does not emulate
 * the RTL8168/8169 that modern consumer boards carry. This driver is therefore
 * tested; an r8169 written here would be a guess with no way to check it, which
 * is the one thing this tree does not ship. The 8169 is recorded as needing the
 * target machine, and the register interface is different enough that it is a
 * separate driver rather than an extension of this one.
 *
 * WHAT IS DIFFERENT ABOUT IT, and it is not a detail: the 8139 has no receive
 * DESCRIPTOR ring. Frames are appended into one circular buffer, each behind a
 * four-byte header, and the driver walks it with a read pointer the hardware
 * can see. Everything else here follows from that.
 *
 * Registers are reached through BAR1, the MEMORY window, not BAR0's I/O ports.
 * Port I/O is x86-only and this kernel keeps it out of anything above the arch
 * layer; the card offers both and only one of them is portable.
 */

#include "net/net.h"
#include "drivers/bus/pci.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/spinlock.h"

/* ---- registers (BAR1, MMIO) --------------------------------------------- */
#define RTL_IDR0     0x00      /* station address, 6 bytes */
#define RTL_MAR0     0x08      /* multicast filter, 8 bytes */
#define RTL_TSD0     0x10      /* transmit status, 4 slots, 4 bytes apart */
#define RTL_TSAD0    0x20      /* transmit start address, 4 slots */
#define RTL_RBSTART  0x30      /* receive buffer physical address */
#define RTL_CMD      0x37
#define RTL_CAPR     0x38      /* current address of packet read */
#define RTL_IMR      0x3C
#define RTL_ISR      0x3E
#define RTL_TCR      0x40
#define RTL_RCR      0x44
#define RTL_CONFIG1  0x52

#define CMD_BUFE     (1u << 0) /* receive buffer empty */
#define CMD_TE       (1u << 2)
#define CMD_RE       (1u << 3)
#define CMD_RST      (1u << 4)

#define ISR_ROK      (1u << 0)
#define ISR_TOK      (1u << 2)

#define RCR_AAP      (1u << 0) /* accept all (promiscuous) -- not used */
#define RCR_APM      (1u << 1) /* accept physical match */
#define RCR_AM       (1u << 2) /* accept multicast */
#define RCR_AB       (1u << 3) /* accept broadcast -- DHCP needs it */
#define RCR_WRAP     (1u << 7) /* let a frame run past the end of the ring */

#define TSD_OWN      (1u << 13)
#define TSD_TOK      (1u << 15)

/* THE RECEIVE RING, and why it is this size. RCR_WRAP tells the card it may
 * write a frame that starts near the end straight past the end rather than
 * splitting it -- which is what makes the walk below a simple pointer bump
 * instead of a reassembly. The cost is that the buffer must have room for one
 * whole frame beyond its nominal length. 8 KiB + 16 bytes of slack + a full
 * frame is the allocation the datasheet asks for. */
#define RTL_RX_LEN   8192
#define RTL_RX_SLACK (16 + 2048)
#define RTL_TX_SLOTS 4
#define RTL_TX_SZ    2048

static uint8_t g_rx[RTL_RX_LEN + RTL_RX_SLACK] __attribute__((aligned(16)));
static uint8_t g_tx[RTL_TX_SLOTS][RTL_TX_SZ]   __attribute__((aligned(16)));

static volatile uint8_t *g_io;
static uint32_t  g_rx_ptr;       /* our read offset into g_rx */
static uint32_t  g_tx_slot;
static spinlock_t g_tx_lock = SPINLOCK_INIT;
static int g_present;

static inline uint8_t  r8 (uint32_t o)            { return *(volatile uint8_t  *)(g_io + o); }
static inline uint16_t r16(uint32_t o)            { return *(volatile uint16_t *)(g_io + o); }
static inline uint32_t r32(uint32_t o)            { return *(volatile uint32_t *)(g_io + o); }
static inline void     w8 (uint32_t o, uint8_t v) { *(volatile uint8_t  *)(g_io + o) = v; }
static inline void     w16(uint32_t o, uint16_t v){ *(volatile uint16_t *)(g_io + o) = v; }
static inline void     w32(uint32_t o, uint32_t v){ *(volatile uint32_t *)(g_io + o) = v; }
static inline uint64_t dma(const volatile void *p){ return KV2P((uint64_t)(uintptr_t)p); }

static const struct pci_device *rtl_find(void) {
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d && d->vendor_id == 0x10EC && d->device_id == 0x8139) return d;
    }
    return 0;
}

bool rtl8139_init(uint8_t mac_out[ETH_ALEN]) {
    const struct pci_device *d = rtl_find();
    if (!d) return false;

    /* BAR1 is the memory window. A card wired for I/O only is refused rather
     * than reached through port I/O, which would put x86 instructions in a
     * file that both architectures compile. */
    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 1);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("rtl8139: no MMIO BAR1 -- this driver does not do port I/O\n");
        return false;
    }
    g_io = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                       bar.size ? bar.size : 0x100);
    if (!g_io) { kprintf("rtl8139: could not map BAR1\n"); return false; }

    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* Out of low-power mode first: after a cold start the card answers
     * register writes and quietly does nothing with them until this is clear. */
    w8(RTL_CONFIG1, 0x00);

    w8(RTL_CMD, CMD_RST);
    for (int spin = 0; spin < 1000000 && (r8(RTL_CMD) & CMD_RST); spin++)
        __asm__ volatile("" ::: "memory");
    if (r8(RTL_CMD) & CMD_RST) { kprintf("rtl8139: reset never completed\n"); return false; }

    for (int i = 0; i < ETH_ALEN; i++) mac_out[i] = r8(RTL_IDR0 + (uint32_t)i);

    w32(RTL_RBSTART, (uint32_t)dma(g_rx));
    g_rx_ptr = 0;

    /* Accept what is addressed to us, to everyone, and to multicast groups --
     * and NOT everything on the wire: promiscuous mode is a different feature
     * with a different cost, and a stack that quietly sees its neighbours'
     * traffic is not one anybody asked for. */
    w32(RTL_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);
    w32(RTL_TCR, 0x03000700);         /* default IFG, 16-byte DMA burst */

    for (int i = 0; i < RTL_TX_SLOTS; i++)
        w32(RTL_TSAD0 + (uint32_t)i * 4, (uint32_t)dma(g_tx[i]));
    g_tx_slot = 0;

    w16(RTL_IMR, 0);                  /* polled, like the other drivers here */
    w16(RTL_ISR, 0xFFFF);             /* write-one-to-clear whatever reset left */
    w8(RTL_CMD, CMD_RE | CMD_TE);

    kprintf("rtl8139: up, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5]);
    g_present = 1;
    return true;
}

int rtl8139_tx(const void *frame, uint32_t len) {
    if (!g_present || !frame || len == 0 || len > RTL_TX_SZ) return -1;

    spin_lock(&g_tx_lock);
    uint32_t i = g_tx_slot;
    uint32_t tsd = RTL_TSD0 + i * 4;

    /* OWN is set by the card when it has finished with the slot. Four slots is
     * not many; refusing is better than overwriting a frame in flight. */
    if (!(r32(tsd) & TSD_OWN) && r32(tsd) != 0) { spin_unlock(&g_tx_lock); return -1; }

    memcpy(g_tx[i], frame, len);
    /* The card will not send a runt. Pad to the 60 bytes an Ethernet frame
     * needs before its CRC rather than letting it drop silently. */
    uint32_t n = len < 60 ? 60 : len;
    if (n > len) memset(g_tx[i] + len, 0, n - len);

    __sync_synchronize();
    w32(RTL_TSAD0 + i * 4, (uint32_t)dma(g_tx[i]));
    w32(tsd, n);                      /* clears OWN and starts the transmit */

    g_tx_slot = (i + 1) % RTL_TX_SLOTS;

    for (int spin = 0; spin < 1000000 && !(r32(tsd) & TSD_OWN); spin++)
        __asm__ volatile("" ::: "memory");

    int ok = (r32(tsd) & TSD_TOK) ? (int)len : -1;
    spin_unlock(&g_tx_lock);
    return ok;
}

/* Is the cable in?
 *
 * MSR bit 2 is LINKB -- "link bad" -- so it is the INVERSE of what the name
 * would suggest and the sense is easy to ship backwards. Checked against QEMU's
 * `set_link`, which unplugs the virtual cable, rather than read off a datasheet
 * and believed. */
#define RTL_MSR   0x58
#define MSR_LINKB (1u << 2)
int rtl8139_link(void) {
    if (!g_present) return -1;
    return (r8(RTL_MSR) & MSR_LINKB) ? 0 : 1;
}

/* Walk the circular buffer until the card says it is empty.
 *
 * Each frame is a four-byte header -- status in the low half, LENGTH INCLUDING
 * THE CRC in the high half -- followed by the bytes. The read pointer advances
 * past both, rounded up to four, and CAPR is written sixteen bytes BEHIND it:
 * that offset is the card's, not a mistake, and getting it wrong reads the same
 * frame forever. */
void rtl8139_poll(void) {
    if (!g_present) return;

    while (!(r8(RTL_CMD) & CMD_BUFE)) {
        uint32_t off = g_rx_ptr % RTL_RX_LEN;
        uint32_t hdr = *(volatile uint32_t *)(g_rx + off);
        uint16_t status = (uint16_t)(hdr & 0xFFFF);
        uint16_t len    = (uint16_t)(hdr >> 16);

        /* A length outside the ring means the header is not a header -- the
         * walk has lost its place, and continuing would hand the stack
         * whatever happened to be in memory. Resync instead of guessing. */
        if (len < 4 || len > RTL_RX_LEN) {
            g_rx_ptr = 0;
            w16(RTL_CAPR, (uint16_t)(0 - 16));
            break;
        }

        if ((status & ISR_ROK) && len >= ETH_HLEN + 4)
            net_rx(g_rx + off + 4, (uint32_t)len - 4);   /* drop the CRC */

        g_rx_ptr = (off + len + 4 + 3) & ~3u;
        g_rx_ptr %= RTL_RX_LEN;
        w16(RTL_CAPR, (uint16_t)(g_rx_ptr - 16));
    }
}
