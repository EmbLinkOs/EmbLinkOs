/* Physical PCI Ethernet: Intel 8254x/82574 legacy descriptors and RTL8139.
 * Intel 8254x Software Developer's Manual: receive/transmit descriptor rings,
 * CTRL/RCTL/TCTL and ring registers; RTL8139C(L) datasheet: register map and
 * receive ring/CAPR. Layouts checked against QEMU's e1000/rtl8139 models.
 * Polling uses the stack's existing 10ms RX wakeup; device IRQs stay masked.
 * Stack callers hold net_lock, so one synchronous TX buffer is sufficient. */
#include "net/net.h"
#include "drivers/bus/pci.h"
#include "drivers/timer/timer.h"
#include "include/io.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define RING 32
#define BUFSIZE 2048
struct intel_rx {
    uint64_t addr;
    uint16_t len, checksum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));
struct intel_tx {
    uint64_t addr;
    uint16_t len;
    uint8_t checksum_offset, command, status, checksum_start;
    uint16_t special;
} __attribute__((packed));
_Static_assert(sizeof(struct intel_rx) == 16, "e1000 RX descriptor");
_Static_assert(sizeof(struct intel_tx) == 16, "e1000 TX descriptor");
static struct {
    volatile uint8_t *regs;
    volatile struct intel_rx rx[RING] __attribute__((aligned(128)));
    volatile struct intel_tx tx[RING] __attribute__((aligned(128)));
    uint8_t receive[RING][BUFSIZE] __attribute__((aligned(4096)));
    uint8_t transmit[BUFSIZE] __attribute__((aligned(4096)));
    unsigned rx_next, tx_next;
} intel;
static struct {
    uint16_t port, rx_pos;
    unsigned tx_next;
    uint8_t receive[8192 + 16 + BUFSIZE] __attribute__((aligned(4096)));
    uint8_t transmit[BUFSIZE] __attribute__((aligned(4096)));
} rtl;
static unsigned active; /* 1=Intel, 2=RTL8139; zero after a DMA/TX failure */

static uint32_t ir(uint32_t off) { return *(volatile uint32_t *)(intel.regs + off); }
static void iw(uint32_t off, uint32_t value) { *(volatile uint32_t *)(intel.regs + off) = value; }
static void dma_publish(void) { __asm__ volatile("sfence" ::: "memory"); }
static void dma_acquire(void) { __asm__ volatile("lfence" ::: "memory"); }
static bool intel_id(uint16_t id) {
    return id == 0x100e || id == 0x100f || id == 0x1010 || id == 0x1011
        || id == 0x1012 || id == 0x1013 || id == 0x1015 || id == 0x1016
        || id == 0x1017 || id == 0x107c || id == 0x10d3;
}
static bool intel_init(const struct pci_device *p, uint8_t *mac) {
    struct pci_bar bar = pci_read_bar(p->bus, p->device, p->function, 0);
    if (!bar.valid || !bar.is_mmio || bar.size < 0x6000) return false;
    intel.regs = (volatile uint8_t *)vmm_map_mmio(bar.address, bar.size);
    if (!intel.regs) return false;
    pci_write16(p->bus, p->device, p->function, PCI_COMMAND,
        pci_read16(p->bus, p->device, p->function, PCI_COMMAND) | 6);
    iw(0xd8, 0xffffffff);    /* IMC */
    iw(0, ir(0) | (1U << 26));
    uint64_t start = time_get_us();
    while (ir(0) & (1U << 26)) {
        if (time_get_us() - start > 100000) return false;
        __asm__ volatile("pause");
    }
    iw(0xd8, 0xffffffff); (void)ir(0xc0);
    /* Allow EEPROM auto-read after reset. RAH.AV marks the loaded MAC valid. */
    start = time_get_us();
    while (!(ir(0x5404) & (1U << 31))) {
        if (time_get_us() - start > 100000) return false;
        __asm__ volatile("pause");
    }
    uint32_t lo = ir(0x5400), hi = ir(0x5404);
    for (unsigned i = 0; i < 4; i++) mac[i] = lo >> (8 * i);
    mac[4] = hi; mac[5] = hi >> 8;
    if ((mac[0] & 1) || !(lo | (hi & 0xffff))) return false;
    for (unsigned i = 0; i < 128; i++) iw(0x5200 + i * 4, 0); /* MTA */
    for (unsigned i = 0; i < RING; i++) {
        intel.rx[i].addr = KV2P(intel.receive[i]);
        intel.rx[i].status = 0;
        intel.tx[i].addr = KV2P(intel.transmit);
        intel.tx[i].status = 1;
    }
    uint64_t rx = KV2P(intel.rx), tx = KV2P(intel.tx);
    iw(0x2800, rx); iw(0x2804, rx >> 32); iw(0x2808, sizeof intel.rx);
    iw(0x2810, 0); iw(0x2818, RING - 1);
    iw(0x3800, tx); iw(0x3804, tx >> 32); iw(0x3808, sizeof intel.tx);
    iw(0x3810, 0); iw(0x3818, 0);
    iw(0x410, 0x0060200a); /* TIPG: Ethernet interpacket gap */
    if (p->device_id == 0x10d3) {
        iw(0x2828, (1U << 25) | (1U << 16) | (1U << 8) | 8);
        iw(0x3828, (1U << 25) | (1U << 16) | (1U << 8) | 8);
    }
    dma_publish();
    iw(0, ir(0) | (1U << 6));   /* CTRL.SLU: auto-negotiated link */
    iw(0x400, 2 | 8 | (0x10 << 4) | (64 << 12));
    iw(0x100, 2 | (1U << 15) | (1U << 26)); /* RX, broadcast, strip CRC */
    kprintf("e1000: Intel %04x, DMA rings ready (polling)\n", p->device_id);
    return true;
}
static void rtl_reset_rx(void) {
    outb(rtl.port + 0x37, 4);
    rtl.rx_pos = 0;
    outl(rtl.port + 0x30, KV2P(rtl.receive));
    outw(rtl.port + 0x38, 0xfff0);
    outl(rtl.port + 0x44, (7 << 13) | (7 << 8) | 0x0a);
    outw(rtl.port + 0x3e, 0xffff);
    outb(rtl.port + 0x37, 0x0c);
}
static bool rtl_init(const struct pci_device *p, uint8_t *mac) {
    struct pci_bar bar = pci_read_bar(p->bus, p->device, p->function, 0);
    if (!bar.valid || bar.is_mmio || bar.address > 0xff00 || bar.size < 0x100
        || KV2P(rtl.receive) + sizeof rtl.receive > 0x100000000ULL
        || KV2P(rtl.transmit) + sizeof rtl.transmit > 0x100000000ULL) return false;
    rtl.port = bar.address;
    pci_write16(p->bus, p->device, p->function, PCI_COMMAND,
        pci_read16(p->bus, p->device, p->function, PCI_COMMAND) | 5);
    outb(rtl.port + 0x52, 0); /* Config1: leave power-down */
    outb(rtl.port + 0x37, 0x10);
    uint64_t start = time_get_us();
    while (inb(rtl.port + 0x37) & 0x10) {
        if (time_get_us() - start > 100000) return false;
        __asm__ volatile("pause");
    }
    outw(rtl.port + 0x3c, 0); /* IMR */
    for (unsigned i = 0; i < 6; i++) mac[i] = inb(rtl.port + i);
    if ((mac[0] & 1) || !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) return false;
    for (unsigned i = 0; i < 4; i++) outl(rtl.port + 0x20 + 4 * i, KV2P(rtl.transmit));
    outl(rtl.port + 0x40, (3U << 24) | (7 << 8));
    dma_publish(); rtl_reset_rx();
    kprintf("rtl8139: Realtek 8139, DMA ring ready (polling)\n");
    return true;
}
bool pci_net_init(uint8_t mac[ETH_ALEN]) {
    if (!tsc_get_freq_hz()) return false;
    for (unsigned i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *p = pci_get_device(i);
        if (p->class_code != 2 || p->subclass != 0) continue;
        if (p->vendor_id == 0x8086 && intel_id(p->device_id) && intel_init(p, mac)) {
            active = 1; return true;
        }
        if (p->vendor_id == 0x10ec && p->device_id == 0x8139 && rtl_init(p, mac)) {
            active = 2; return true;
        }
    }
    return false;
}
int pci_net_tx(const void *frame, uint32_t len) {
    if (!active || !frame || len < ETH_HLEN || len > ETH_FRAME_MAX) return -1;
    uint32_t wire = len < 60 ? 60 : len;
    uint64_t start = time_get_us();
    if (active == 1) {
        unsigned i = intel.tx_next;
        memcpy(intel.transmit, frame, len);
        if (wire > len) memset(intel.transmit + len, 0, wire - len);
        intel.tx[i].len = wire;
        intel.tx[i].command = 1 | 2 | 8; /* EOP, insert FCS, report status */
        intel.tx[i].status = 0;
        dma_publish();
        intel.tx_next = (i + 1) % RING;
        iw(0x3818, intel.tx_next);
        while (!(intel.tx[i].status & 1)) {
            if (time_get_us() - start > 1000000) {
                iw(0x100, 0); iw(0x400, 0); active = 0;
                kprintf("e1000: TX timeout; interface stopped\n"); return -1;
            }
            __asm__ volatile("pause");
        }
        dma_acquire();
        return (intel.tx[i].status & 0x0e) ? -1 : 0;
    }
    unsigned i = rtl.tx_next;
    memcpy(rtl.transmit, frame, len);
    if (wire > len) memset(rtl.transmit + len, 0, wire - len);
    dma_publish();
    outl(rtl.port + 0x10 + i * 4, (8 << 16) | wire);
    rtl.tx_next = (i + 1) % 4;
    for (;;) {
        uint32_t s = inl(rtl.port + 0x10 + i * 4);
        if (s & (0x8000 | 0x40000000 | 0x80000000)) return (s & 0x8000) ? 0 : -1;
        if (time_get_us() - start > 1000000) {
            outb(rtl.port + 0x37, 0); active = 0;
            kprintf("rtl8139: TX timeout; interface stopped\n"); return -1;
        }
        __asm__ volatile("pause");
    }
}
void pci_net_poll(void) {
    uint8_t frame[ETH_FRAME_MAX];
    if (active == 1) {
        for (unsigned n = 0; n < RING; n++) {
            unsigned i = intel.rx_next;
            if (!(intel.rx[i].status & 1)) break;
            dma_acquire();
            uint16_t len = intel.rx[i].len;
            bool valid = (intel.rx[i].status & 2) && !intel.rx[i].errors
                      && len >= ETH_HLEN && len <= sizeof frame;
            if (valid) memcpy(frame, intel.receive[i], len);
            intel.rx[i].status = 0;
            dma_publish(); iw(0x2818, i);
            intel.rx_next = (i + 1) % RING;
            if (valid) net_rx(frame, len);
        }
    } else if (active == 2) {
        if (inw(rtl.port + 0x3e) & 0x50) { rtl_reset_rx(); return; }
        for (unsigned n = 0; n < 32 && !(inb(rtl.port + 0x37) & 1); n++) {
            dma_acquire();
            unsigned at = rtl.rx_pos;
            uint16_t status = rtl.receive[at] | ((uint16_t)rtl.receive[(at + 1) % 8192] << 8);
            uint16_t len = rtl.receive[(at + 2) % 8192] | ((uint16_t)rtl.receive[(at + 3) % 8192] << 8);
            if (len == 0xfff0) break; /* device still writing the packet */
            if (len < 4 || len > ETH_FRAME_MAX + 4) { rtl_reset_rx(); return; }
            bool valid = (status & 1) && !(status & 0x3e) && len >= ETH_HLEN + 4;
            if (valid) for (unsigned j = 0; j < len - 4U; j++) frame[j] = rtl.receive[(at + 4 + j) % 8192];
            rtl.rx_pos = (at + len + 4 + 3) & ~3U;
            rtl.rx_pos %= 8192;
            dma_publish(); outw(rtl.port + 0x38, rtl.rx_pos - 16);
            if (valid) net_rx(frame, len - 4);
        }
        outw(rtl.port + 0x3e, inw(rtl.port + 0x3e));
    }
}
