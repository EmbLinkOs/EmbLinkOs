/* kernel/net/e1000.c -- Intel PRO/1000 (8254x / 8257x), the first NIC on this
 * machine that a real computer might actually contain.
 *
 * WHY THIS EXISTS. Every byte this OS has ever sent went through virtio-net,
 * which is a contract with a hypervisor. A physical machine has no such device,
 * so on the metal the whole stack above -- TCP, DHCP, DNS, TLS, the browser,
 * git -- was unreachable code sitting on top of a driver for hardware that was
 * not there. docs/PILLARS.md calls this out as a phase-1 pillar: "A physical
 * machine has no network at all."
 *
 * WHICH CHIPS. This is the LEGACY descriptor interface, which every Intel
 * gigabit part from the 82540 onwards still implements: the 8254x desktop and
 * server parts, the 8257x (82571/2/3/4) that followed them, and QEMU's `e1000`
 * (an 82540EM) and `e1000e` (an 82574L). It is deliberately not the fastest way
 * to drive any of them -- no TSO, no checksum offload, no MSI-X, no multiple
 * queues -- because the first thing a NIC on a new machine has to be is
 * CORRECT, and every one of those features is a separate way to be subtly
 * wrong. They are additions to a working driver, not prerequisites for one.
 *
 * The PCH parts in laptops since about 2013 (I217/I218/I219) are NOT covered:
 * they split the MAC from the PHY across an internal bus and need a different
 * bring-up. That is a separate driver and is recorded as such.
 *
 * WHERE THE MEMORY LIVES. Rings and buffers are in .bss, like virtio_net.c's,
 * for the same reason: the kernel image is physically contiguous, so KV2P is
 * exact and no DMA buffer straddles a discontiguity. A ring in kmalloc'd memory
 * would need a scatter list the hardware cannot take.
 */

#include "net/net.h"
#include "drivers/bus/pci.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/spinlock.h"

/* ---- registers (BAR0, MMIO) --------------------------------------------- */
#define E1000_CTRL     0x0000
#define E1000_STATUS   0x0008
#define E1000_EERD     0x0014
#define E1000_ICR      0x00C0
#define E1000_IMS      0x00D0
#define E1000_IMC      0x00D8
#define E1000_RCTL     0x0100
#define E1000_TCTL     0x0400
#define E1000_TIPG     0x0410
#define E1000_RDBAL    0x2800
#define E1000_RDBAH    0x2804
#define E1000_RDLEN    0x2808
#define E1000_RDH      0x2810
#define E1000_RDT      0x2818
#define E1000_TDBAL    0x3800
#define E1000_TDBAH    0x3804
#define E1000_TDLEN    0x3808
#define E1000_TDH      0x3810
#define E1000_TDT      0x3818
#define E1000_MTA      0x5200        /* 128 dwords of multicast table */
#define E1000_RAL0     0x5400
#define E1000_RAH0     0x5404

#define CTRL_SLU       (1u << 6)     /* set link up */
#define CTRL_ASDE      (1u << 5)     /* auto-speed detect enable */
#define CTRL_RST       (1u << 26)
#define CTRL_PHY_RST   (1u << 31)

#define STATUS_LU      (1u << 1)     /* link up */

#define RCTL_EN        (1u << 1)
#define RCTL_SBP       (1u << 2)     /* store bad packets */
#define RCTL_UPE       (1u << 3)     /* unicast promiscuous */
#define RCTL_MPE       (1u << 4)     /* multicast promiscuous */
#define RCTL_LPE       (1u << 5)     /* long packet enable */
#define RCTL_BAM       (1u << 15)    /* broadcast accept -- DHCP needs it */
#define RCTL_BSIZE_2048 0            /* with BSEX clear */
#define RCTL_SECRC     (1u << 26)    /* strip the ethernet CRC */

#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)     /* pad short packets */
#define TCTL_CT_SHIFT  4             /* collision threshold */
#define TCTL_COLD_SHIFT 12           /* collision distance */

#define TXD_CMD_EOP    (1u << 0)     /* end of packet */
#define TXD_CMD_IFCS   (1u << 1)     /* insert FCS */
#define TXD_CMD_RS     (1u << 3)     /* report status */
#define TXD_STA_DD     (1u << 0)     /* descriptor done */

#define RXD_STA_DD     (1u << 0)
#define RXD_STA_EOP    (1u << 1)

/* ---- rings -------------------------------------------------------------- */
/* Counts are powers of two so the head/tail arithmetic is a mask, and the byte
 * lengths land on the 128-byte multiple the hardware requires for RDLEN/TDLEN
 * (32 * 16 = 512, 16 * 16 = 256). */
#define E1000_RX_DESCS 32
#define E1000_TX_DESCS 16
#define E1000_BUF_SZ   2048          /* matches RCTL_BSIZE_2048 */

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* The hardware reads these, so they are aligned as it requires (16 bytes for
 * the rings) and live where KV2P is exact. */
static struct e1000_rx_desc g_rx_ring[E1000_RX_DESCS] __attribute__((aligned(16)));
static struct e1000_tx_desc g_tx_ring[E1000_TX_DESCS] __attribute__((aligned(16)));
static uint8_t g_rx_buf[E1000_RX_DESCS][E1000_BUF_SZ] __attribute__((aligned(16)));
static uint8_t g_tx_buf[E1000_TX_DESCS][E1000_BUF_SZ] __attribute__((aligned(16)));

static volatile uint8_t *g_mmio;
static uint32_t g_rx_next;           /* the descriptor we will look at next */
static uint32_t g_tx_next;
static spinlock_t g_tx_lock = SPINLOCK_INIT;
static int g_present;

static inline uint32_t er(uint32_t off)            { return *(volatile uint32_t *)(g_mmio + off); }
static inline void     ew(uint32_t off, uint32_t v){ *(volatile uint32_t *)(g_mmio + off) = v; }
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* A short spin. There is no sleeping here: this all runs during bring-up,
 * before the scheduler is anybody's to yield to. */
static void e1000_delay(void) {
    for (volatile int i = 0; i < 200000; i++) { }
}

/* ---- the MAC address ----------------------------------------------------
 *
 * Read from the receive address registers, not the EEPROM. Firmware (and QEMU)
 * loads RAL0/RAH0 from the EEPROM at reset, so this is the same value with no
 * EEPROM state machine to get wrong -- and the EEPROM interface is one of the
 * places the 8254x and 8257x genuinely differ. If RAH0's Address Valid bit is
 * clear the part did not load one, and that is reported rather than papered
 * over with a made-up address. */
static bool e1000_read_mac(uint8_t mac[ETH_ALEN]) {
    uint32_t lo = er(E1000_RAL0), hi = er(E1000_RAH0);
    if (!(hi & (1u << 31))) return false;        /* Address Valid */
    mac[0] = (uint8_t)(lo);       mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16); mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)(hi);       mac[5] = (uint8_t)(hi >> 8);
    return true;
}

static void e1000_rx_init(void) {
    for (int i = 0; i < E1000_RX_DESCS; i++) {
        memset(&g_rx_ring[i], 0, sizeof g_rx_ring[i]);
        g_rx_ring[i].addr = dma(g_rx_buf[i]);
    }
    ew(E1000_RDBAL, (uint32_t)(dma(g_rx_ring) & 0xFFFFFFFFu));
    ew(E1000_RDBAH, (uint32_t)(dma(g_rx_ring) >> 32));
    ew(E1000_RDLEN, (uint32_t)(E1000_RX_DESCS * sizeof(struct e1000_rx_desc)));
    ew(E1000_RDH, 0);
    /* The tail is the last descriptor the driver OWNS. Setting it to the final
     * index hands the hardware every one of them. */
    ew(E1000_RDT, E1000_RX_DESCS - 1);
    g_rx_next = 0;

    ew(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void e1000_tx_init(void) {
    for (int i = 0; i < E1000_TX_DESCS; i++) {
        memset(&g_tx_ring[i], 0, sizeof g_tx_ring[i]);
        g_tx_ring[i].addr   = dma(g_tx_buf[i]);
        g_tx_ring[i].status = TXD_STA_DD;   /* free: nothing in flight yet */
    }
    ew(E1000_TDBAL, (uint32_t)(dma(g_tx_ring) & 0xFFFFFFFFu));
    ew(E1000_TDBAH, (uint32_t)(dma(g_tx_ring) >> 32));
    ew(E1000_TDLEN, (uint32_t)(E1000_TX_DESCS * sizeof(struct e1000_tx_desc)));
    ew(E1000_TDH, 0);
    ew(E1000_TDT, 0);
    g_tx_next = 0;

    ew(E1000_TCTL, TCTL_EN | TCTL_PSP | (0x0F << TCTL_CT_SHIFT) | (0x40 << TCTL_COLD_SHIFT));
    ew(E1000_TIPG, 10 | (8 << 10) | (6 << 20));   /* IEEE 802.3 inter-packet gap */
}

/* Is this PCI function an Intel gigabit part we drive?
 *
 * Matched on VENDOR + CLASS rather than a list of device ids. A device-id table
 * is a list of the parts somebody had on their desk, and the failure mode of
 * missing one is a machine with no network and nothing in the log explaining
 * why. The registers below are the same across the family; a part that is not
 * really compatible fails the link check and says so. */
static const struct pci_device *e1000_find(void) {
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        if (d->vendor_id != 0x8086) continue;
        if (d->class_code != 0x02 || d->subclass != 0x00) continue;   /* ethernet */
        return d;
    }
    return 0;
}

bool e1000_init(uint8_t mac_out[ETH_ALEN]) {
    const struct pci_device *d = e1000_find();
    if (!d) return false;

    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("e1000: %04x:%04x has no MMIO BAR0\n", d->vendor_id, d->device_id);
        return false;
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                         bar.size ? bar.size : 0x20000);
    if (!g_mmio) { kprintf("e1000: could not map BAR0\n"); return false; }

    /* Without bus mastering the part cannot complete a single DMA, and there is
     * no firmware here to have enabled it. */
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* Reset, then wait: the datasheet requires the device be left alone for
     * about a microsecond, and this is a long way more than that. Interrupts
     * are masked first so a reset cannot deliver one into a half-set-up
     * driver. */
    ew(E1000_IMC, 0xFFFFFFFFu);
    ew(E1000_CTRL, er(E1000_CTRL) | CTRL_RST);
    e1000_delay();
    ew(E1000_IMC, 0xFFFFFFFFu);
    (void)er(E1000_ICR);                     /* reading clears it */

    if (!e1000_read_mac(mac_out)) {
        kprintf("e1000: %04x:%04x has no valid station address\n",
                d->vendor_id, d->device_id);
        return false;
    }

    /* Bring the link up and let the part negotiate its own speed. */
    ew(E1000_CTRL, er(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Clear the multicast table: after a reset it is undefined, and a stale
     * entry means accepting traffic that is not ours. */
    for (int i = 0; i < 128; i++) ew(E1000_MTA + i * 4, 0);

    e1000_rx_init();
    e1000_tx_init();

    /* The link takes a moment. It is REPORTED, not waited for -- a cable that
     * is not plugged in yet is not a reason to refuse to initialise, and DHCP
     * above will fail loudly enough if it never comes up. */
    e1000_delay();
    uint32_t st = er(E1000_STATUS);
    kprintf("e1000: %04x:%04x up, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            d->vendor_id, d->device_id,
            mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5],
            (st & STATUS_LU) ? "up" : "down");

    g_present = 1;
    return true;
}

/* Send one Ethernet frame. Synchronous: post the descriptor, kick the tail,
 * and wait for the hardware to say Descriptor Done.
 *
 * Waiting is deliberate at this stage. It costs throughput and it means a
 * caller always knows whether its frame left, which is the property a stack
 * being brought up on new hardware needs most. */
int e1000_tx(const void *frame, uint32_t len) {
    if (!g_present || !frame || len == 0) return -1;
    if (len > E1000_BUF_SZ) return -1;

    spin_lock(&g_tx_lock);
    uint32_t i = g_tx_next;
    struct e1000_tx_desc *td = &g_tx_ring[i];

    /* The slot must be free. If the hardware has not finished with it, this
     * driver has outrun a ring of 16 and the honest answer is to refuse rather
     * than overwrite a frame in flight. */
    if (!(td->status & TXD_STA_DD)) { spin_unlock(&g_tx_lock); return -1; }

    memcpy(g_tx_buf[i], frame, len);
    td->addr   = dma(g_tx_buf[i]);
    td->length = (uint16_t)len;
    td->cso    = 0;
    td->css    = 0;
    td->special = 0;
    td->status = 0;
    td->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

    g_tx_next = (i + 1) % E1000_TX_DESCS;
    __sync_synchronize();
    ew(E1000_TDT, g_tx_next);

    /* Bounded. A hang here would be a hang in whatever asked to send. */
    for (int spin = 0; spin < 1000000 && !(td->status & TXD_STA_DD); spin++)
        __asm__ volatile("" ::: "memory");

    int ok = (td->status & TXD_STA_DD) ? (int)len : -1;
    spin_unlock(&g_tx_lock);
    return ok;
}

/* Drain everything the hardware has left us, and give the descriptors back.
 *
 * The tail is moved to the LAST descriptor handed back, which is what the
 * hardware reads as "the driver owns up to here". Moving it to the next one to
 * be filled instead is the classic off-by-one that leaves the ring one short
 * and eventually wedges it. */
void e1000_poll(void) {
    if (!g_present) return;
    uint32_t last = (uint32_t)-1;

    while (g_rx_ring[g_rx_next].status & RXD_STA_DD) {
        struct e1000_rx_desc *rd = &g_rx_ring[g_rx_next];
        uint16_t len = rd->length;

        /* A frame split across descriptors cannot happen at this MTU with
         * 2048-byte buffers, and if it ever does it is dropped rather than
         * delivered as a truncated packet that looks like a real one. */
        if ((rd->status & RXD_STA_EOP) && !rd->errors && len >= ETH_HLEN &&
            len <= E1000_BUF_SZ)
            net_rx(g_rx_buf[g_rx_next], len);

        rd->status = 0;                      /* hand it back */
        last = g_rx_next;
        g_rx_next = (g_rx_next + 1) % E1000_RX_DESCS;
    }

    if (last != (uint32_t)-1) {
        __sync_synchronize();
        ew(E1000_RDT, last);
    }
}
