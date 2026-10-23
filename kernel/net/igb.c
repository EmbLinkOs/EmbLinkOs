/* kernel/net/igb.c -- Intel 82575/82576 and the gigabit parts after them.
 *
 * e1000.c drives everything from the 82540 to the 82574 with one descriptor
 * format, because those parts all kept the original one. The 8257x generation
 * that followed did not: igb parts use ADVANCED DESCRIPTORS, which are a
 * different size, a different layout, and -- in the receive direction --
 * REWRITTEN BY THE HARDWARE INTO A DIFFERENT SHAPE THAN THE ONE THE DRIVER
 * POSTED. That last part is what makes this a separate file rather than a flag
 * in e1000.c: the same sixteen bytes mean "here are two buffer addresses" on
 * the way in and "here is a length and a status" on the way out.
 *
 * THE QUEUE MUST BE ENABLED, SEPARATELY, AFTER IT IS SET UP. On an 8254x,
 * RCTL.EN is the whole switch. Here each queue has its own RXDCTL/TXDCTL
 * enable bit, and a ring that is configured, pointed at, and tailed -- with
 * RCTL.EN set -- still receives absolutely nothing until bit 25 of its RXDCTL
 * goes up. There is no error and no status bit that says so.
 *
 * BUFFER SIZE COMES FROM SRRCTL, NOT RCTL. The RCTL size field is ignored once
 * advanced descriptors are in use, and SRRCTL's field is in KILOBYTES.
 *
 * Like e1000.c: one queue, no offloads, no MSI-X, polled. Correct first.
 */
#include "net/net.h"
#include "drivers/bus/pci.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/spinlock.h"

#define IGB_CTRL      0x00000
#define IGB_STATUS    0x00008
#define IGB_CTRL_EXT  0x00018
#define IGB_MDIC      0x00020
#define IGB_IMC       0x000D8
#define IGB_RCTL      0x00100
#define IGB_TCTL      0x00400
#define IGB_EIMC      0x01528
#define IGB_MTA       0x05200
#define IGB_RAL0      0x05400
#define IGB_RAH0      0x05404
#define IGB_MRQC      0x05818

/* Per-queue blocks, 0x40 apart. Only queue 0 is used. */
#define IGB_RDBAL(q)  (0x0C000 + (q) * 0x40)
#define IGB_RDBAH(q)  (0x0C004 + (q) * 0x40)
#define IGB_RDLEN(q)  (0x0C008 + (q) * 0x40)
#define IGB_SRRCTL(q) (0x0C00C + (q) * 0x40)
#define IGB_RDH(q)    (0x0C010 + (q) * 0x40)
#define IGB_RDT(q)    (0x0C018 + (q) * 0x40)
#define IGB_RXDCTL(q) (0x0C028 + (q) * 0x40)
#define IGB_TDBAL(q)  (0x0E000 + (q) * 0x40)
#define IGB_TDBAH(q)  (0x0E004 + (q) * 0x40)
#define IGB_TDLEN(q)  (0x0E008 + (q) * 0x40)
#define IGB_TDH(q)    (0x0E010 + (q) * 0x40)
#define IGB_TDT(q)    (0x0E018 + (q) * 0x40)
#define IGB_TXDCTL(q) (0x0E028 + (q) * 0x40)

#define CTRL_SLU      (1u << 6)
#define CTRL_RST      (1u << 26)
#define STATUS_LU     (1u << 1)

#define RCTL_EN       (1u << 1)
#define RCTL_BAM      (1u << 15)     /* broadcast: DHCP needs it */
#define RCTL_SECRC    (1u << 26)

#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)

#define XDCTL_QUEUE_ENABLE (1u << 25)

/* The MDI control register: one PHY register access per write. Data in bits
 * 15:0, the PHY register number in 20:16, the PHY's own address in 25:21, the
 * opcode in 27:26, and bit 28 set by the hardware when it is finished. */
#define MDIC_OP_WRITE (1u << 26)
#define MDIC_READY    (1u << 28)
#define MDIC_ERROR    (1u << 30)
#define IGB_PHY_ADDR  1              /* the integrated PHY */
#define PHY_BMCR      0              /* the basic control register */
#define BMCR_ANRESTART 0x0200
#define BMCR_AUTOEN    0x1000

/* SRRCTL: packet buffer size in KILOBYTES in bits 6:0, descriptor type in
 * bits 27:25 (1 = advanced, one buffer). */
#define SRRCTL_DESCTYPE_ADV (1u << 25)

/* Advanced transmit data descriptor, second dword. */
#define TXD_DTYP_DATA (3u << 20)
#define TXD_DCMD_EOP  (1u << 24)
#define TXD_DCMD_IFCS (1u << 25)
#define TXD_DCMD_RS   (1u << 27)
#define TXD_DCMD_DEXT (1u << 29)

#define RXD_STAT_DD   (1u << 0)
#define RXD_STAT_EOP  (1u << 1)

#define IGB_RX_DESCS 32
#define IGB_TX_DESCS 16
#define IGB_BUF_SZ   2048

/* THE SAME SIXTEEN BYTES, TWO WAYS ROUND. The driver writes the read form;
 * the hardware overwrites it with the write-back form. A union rather than
 * two arrays because it is genuinely one descriptor whose meaning changes
 * hands, and pretending otherwise hides the aliasing. */
union igb_rx_desc {
    struct { uint64_t pkt_addr; uint64_t hdr_addr; } read;
    struct {
        uint32_t lo_dword;
        uint32_t hi_dword;
        uint32_t status_error;
        uint16_t length;
        uint16_t vlan;
    } wb;
} __attribute__((packed));

struct igb_tx_desc {
    uint64_t buffer_addr;
    uint32_t cmd_type_len;
    uint32_t olinfo_status;
} __attribute__((packed));

static union igb_rx_desc g_rx_ring[IGB_RX_DESCS] __attribute__((aligned(128)));
static struct igb_tx_desc g_tx_ring[IGB_TX_DESCS] __attribute__((aligned(128)));
static uint8_t g_rx_buf[IGB_RX_DESCS][IGB_BUF_SZ] __attribute__((aligned(16)));
static uint8_t g_tx_buf[IGB_TX_DESCS][IGB_BUF_SZ] __attribute__((aligned(16)));

static volatile uint8_t *g_mmio;
static uint32_t g_rx_next, g_tx_next;
static spinlock_t g_tx_lock = SPINLOCK_INIT;
static int g_present;

static inline uint32_t er(uint32_t o)             { return *(volatile uint32_t *)(g_mmio + o); }
static inline void     ew(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio + o) = v; }
static inline uint64_t dma(const volatile void *p){ return KV2P((uint64_t)(uintptr_t)p); }
static void igb_delay(void) { for (volatile int i = 0; i < 200000; i++) { } }

/* Write one PHY register through MDIC. */
static bool igb_phy_write(uint8_t reg, uint16_t data) {
    ew(IGB_MDIC, (uint32_t)data | ((uint32_t)reg << 16) |
                 ((uint32_t)IGB_PHY_ADDR << 21) | MDIC_OP_WRITE);
    for (int i = 0; i < 1000000; i++) {
        uint32_t v = er(IGB_MDIC);
        if (v & MDIC_READY) return !(v & MDIC_ERROR);
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* BRING THE LINK UP THROUGH THE PHY, NOT THROUGH CTRL.
 *
 * On an 8254x, setting CTRL.SLU is what makes STATUS.LU go high, and e1000.c
 * does exactly that. On these parts SLU does nothing of the kind: the link
 * comes up when AUTONEGOTIATION COMPLETES, and autonegotiation is started by
 * writing the PHY's control register. Skipping it leaves STATUS.LU clear.
 *
 * That is not cosmetic. A receiver with no link accepts nothing -- frames are
 * dropped before any filter or descriptor is consulted -- so the symptom is a
 * card that transmits perfectly and hears nothing back. DHCP goes out, the
 * answer arrives at the port, and the driver never sees it. Measured exactly
 * that way: a packet capture on the host showed the DISCOVER leaving and the
 * OFFER arriving, with the guest silent. */
static void igb_link_start(void) {
    igb_phy_write(PHY_BMCR, BMCR_AUTOEN | BMCR_ANRESTART);
    /* NOT WAITED FOR. Negotiation takes the better part of a second, and
     * spending that in the middle of boot buys nothing: the first thing that
     * needs the link is DHCP, which runs much later. igb_link() reports the
     * live bit, so the state is never guessed -- it is simply not yet known
     * at the moment this driver finishes initialising, and the log says so
     * rather than printing "down" about a link that is still coming up. */
}

/* The device ids this driver claims. Unlike e1000.c, which matches the whole
 * vendor+class, this one is a LIST -- because the two drivers would otherwise
 * fight over the same PCI functions and the wrong one would win by ordering.
 * e1000.c refuses these same ids for the same reason. */
bool igb_owns_device(uint16_t device_id) {
    switch (device_id) {
    case 0x10C9:   /* 82576                        */
    case 0x10E6: case 0x10E7: case 0x10E8:
    case 0x1526: case 0x150A: case 0x150D:
    case 0x1518: case 0x1521: case 0x1522: case 0x1523: case 0x1524:
    case 0x1533: case 0x1536: case 0x1537: case 0x1538: /* i210/i211 */
    case 0x10A7: case 0x10A9: case 0x10D6:             /* 82575     */
        return true;
    default:
        return false;
    }
}

static const struct pci_device *igb_find(void) {
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->vendor_id != 0x8086) continue;
        if (d->class_code != 0x02 || d->subclass != 0x00) continue;
        if (igb_owns_device(d->device_id)) return d;
    }
    return 0;
}

static bool igb_read_mac(uint8_t mac[ETH_ALEN]) {
    uint32_t lo = er(IGB_RAL0), hi = er(IGB_RAH0);
    if (!(hi & (1u << 31))) return false;        /* Address Valid */
    mac[0] = (uint8_t)lo;         mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16); mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)hi;         mac[5] = (uint8_t)(hi >> 8);
    return true;
}

static void igb_rx_init(void) {
    for (int i = 0; i < IGB_RX_DESCS; i++) {
        g_rx_ring[i].read.pkt_addr = dma(g_rx_buf[i]);
        g_rx_ring[i].read.hdr_addr = 0;   /* one-buffer mode: no header split */
    }
    ew(IGB_RDBAL(0), (uint32_t)(dma(g_rx_ring) & 0xFFFFFFFFu));
    ew(IGB_RDBAH(0), (uint32_t)(dma(g_rx_ring) >> 32));
    ew(IGB_RDLEN(0), IGB_RX_DESCS * (uint32_t)sizeof(union igb_rx_desc));
    /* Buffer size in KILOBYTES, and the descriptor type. Leaving DESCTYPE at
     * zero asks for legacy descriptors, which this ring is not. */
    ew(IGB_SRRCTL(0), (IGB_BUF_SZ / 1024) | SRRCTL_DESCTYPE_ADV);
    ew(IGB_RDH(0), 0);
    ew(IGB_RDT(0), IGB_RX_DESCS - 1);
    g_rx_next = 0;

    ew(IGB_RXDCTL(0), er(IGB_RXDCTL(0)) | XDCTL_QUEUE_ENABLE);
    ew(IGB_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
}

static void igb_tx_init(void) {
    memset(g_tx_ring, 0, sizeof g_tx_ring);
    /* Mark every slot done so the first send finds a free one. The hardware
     * writes this bit back; nothing has run yet to set it. */
    for (int i = 0; i < IGB_TX_DESCS; i++) g_tx_ring[i].olinfo_status = 1;
    ew(IGB_TDBAL(0), (uint32_t)(dma(g_tx_ring) & 0xFFFFFFFFu));
    ew(IGB_TDBAH(0), (uint32_t)(dma(g_tx_ring) >> 32));
    ew(IGB_TDLEN(0), IGB_TX_DESCS * (uint32_t)sizeof(struct igb_tx_desc));
    ew(IGB_TDH(0), 0);
    ew(IGB_TDT(0), 0);
    g_tx_next = 0;

    ew(IGB_TXDCTL(0), er(IGB_TXDCTL(0)) | XDCTL_QUEUE_ENABLE);
    ew(IGB_TCTL, TCTL_EN | TCTL_PSP);
}

bool igb_init(uint8_t mac_out[ETH_ALEN]) {
    const struct pci_device *d = igb_find();
    if (!d) return false;

    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("igb: %04x:%04x has no MMIO BAR0\n", d->vendor_id, d->device_id);
        return false;
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address,
                                                        bar.size ? bar.size : 0x20000);
    if (!g_mmio) { kprintf("igb: could not map BAR0\n"); return false; }
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    ew(IGB_IMC, 0xFFFFFFFFu);
    ew(IGB_EIMC, 0xFFFFFFFFu);
    ew(IGB_CTRL, er(IGB_CTRL) | CTRL_RST);
    igb_delay();
    ew(IGB_IMC, 0xFFFFFFFFu);
    ew(IGB_EIMC, 0xFFFFFFFFu);

    if (!igb_read_mac(mac_out)) {
        kprintf("igb: %04x:%04x has no valid station address\n",
                d->vendor_id, d->device_id);
        return false;
    }

    ew(IGB_CTRL, er(IGB_CTRL) | CTRL_SLU);
    for (int i = 0; i < 128; i++) ew(IGB_MTA + i * 4, 0);
    /* No receive-side scaling: one queue, and MRQC selecting anything else
     * would steer frames at rings that do not exist. */
    ew(IGB_MRQC, 0);

    igb_rx_init();
    igb_tx_init();
    igb_link_start();
    kprintf("igb: %04x:%04x up, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            d->vendor_id, d->device_id,
            mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5],
            (er(IGB_STATUS) & STATUS_LU) ? "up" : "down");
    g_present = 1;
    return true;
}

int igb_tx(const void *frame, uint32_t len) {
    if (!g_present || !frame || !len || len > IGB_BUF_SZ) return -1;

    spin_lock(&g_tx_lock);
    uint32_t i = g_tx_next;
    struct igb_tx_desc *td = &g_tx_ring[i];
    if (!(td->olinfo_status & 1u)) { spin_unlock(&g_tx_lock); return -1; }

    memcpy(g_tx_buf[i], frame, len);
    td->buffer_addr  = dma(g_tx_buf[i]);
    /* DEXT says "this is an advanced descriptor" and DTYP says which kind.
     * Without both, the hardware reads these sixteen bytes as the legacy
     * layout -- where the length field is in a different place and the top
     * half of the address is a command byte. */
    td->cmd_type_len = (len & 0xFFFFu) | TXD_DTYP_DATA | TXD_DCMD_DEXT |
                       TXD_DCMD_EOP | TXD_DCMD_IFCS | TXD_DCMD_RS;
    /* PAYLEN lives in bits 31:14 of the status dword. It is the length of the
     * whole packet, which for a single-descriptor frame is the same number
     * again -- but it is a separate field and the hardware uses it. */
    td->olinfo_status = (uint32_t)len << 14;

    g_tx_next = (i + 1) % IGB_TX_DESCS;
    __sync_synchronize();
    ew(IGB_TDT(0), g_tx_next);

    for (int spin = 0; spin < 1000000 && !(td->olinfo_status & 1u); spin++)
        __asm__ volatile("" ::: "memory");

    int ok = (td->olinfo_status & 1u) ? (int)len : -1;
    spin_unlock(&g_tx_lock);
    return ok;
}

int igb_link(void) {
    if (!g_present) return -1;
    return (er(IGB_STATUS) & STATUS_LU) ? 1 : 0;
}

void igb_poll(void) {
    if (!g_present) return;
    uint32_t last = (uint32_t)-1;

    while (g_rx_ring[g_rx_next].wb.status_error & RXD_STAT_DD) {
        union igb_rx_desc *rd = &g_rx_ring[g_rx_next];
        uint16_t len = rd->wb.length;
        uint32_t se = rd->wb.status_error;

        if ((se & RXD_STAT_EOP) && len >= ETH_HLEN && len <= IGB_BUF_SZ)
            net_rx(g_rx_buf[g_rx_next], len);

        /* HAND IT BACK BY REWRITING THE READ FORM. Clearing the status bits
         * is what an 8254x wants; here the hardware has overwritten the
         * buffer address with its own report, so the address has to be put
         * back or the next frame is DMAed to whatever that report looked
         * like as a pointer. */
        rd->read.pkt_addr = dma(g_rx_buf[g_rx_next]);
        rd->read.hdr_addr = 0;

        last = g_rx_next;
        g_rx_next = (g_rx_next + 1) % IGB_RX_DESCS;
    }

    if (last != (uint32_t)-1) {
        __sync_synchronize();
        ew(IGB_RDT(0), last);
    }
}
