/* kernel/net/i8255x.c -- Intel PRO/100 (82557/82558/82559 and the 82550/1/2).
 *
 * This is the card in every business desktop and laptop built between about
 * 1996 and 2005, and it is NOT a small e1000. The gigabit parts are driven by
 * writing descriptors into a ring and moving a tail pointer. This one is
 * driven by building a LINKED LIST OF COMMAND BLOCKS in memory and telling a
 * small processor on the card to go and execute it. Transmitting a frame,
 * setting the station address and configuring the receive filter are all the
 * same mechanism -- they are different opcodes in the same command block.
 *
 * The card has two independent units, and they are started differently:
 *
 *   - THE COMMAND UNIT (CU) runs the list described above. Its list must be
 *     terminated, or it runs off into whatever memory follows.
 *   - THE RECEIVE UNIT (RU) walks a second list of frame descriptors and
 *     fills them. It STOPS when it reaches the one marked end-of-list, and it
 *     does not restart on its own. A driver that forgets to re-arm it
 *     receives exactly as many frames as it posted descriptors and then goes
 *     permanently deaf, with no error anywhere.
 *
 * THE MAC ADDRESS IS ONLY IN THE EEPROM. There is no register that holds it
 * after reset, unlike every other card here -- so this driver bit-bangs a
 * three-wire serial EEPROM to get it. That is the ugliest forty lines in the
 * file and there is no way around them.
 *
 * Addresses in command blocks are PHYSICAL AND 32-BIT. Everything the card
 * touches therefore has to live below 4 GiB, which in this kernel it does:
 * the rings are in .bss and the image is loaded low.
 */
#include "net/net.h"
#include "drivers/bus/pci.h"
#include "include/kprintf.h"
#include "include/kstring.h"
/* THE I/O-PORT FALLBACK IS x86-ONLY. This card exposes the same registers
 * twice -- once as memory and once as I/O ports -- and the second window only
 * means anything on a machine that HAS an I/O port space. aarch64 does not,
 * so there the memory window is the only one and a card without it is
 * refused rather than reached for through an instruction that does not
 * exist. */
#if defined(__x86_64__)
#include "include/io.h"
#endif
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/spinlock.h"
#include "include/arch_irq.h"       /* arch_cpu_relax */

/* ---- the control/status block (CSR), the card's only registers ---------- */
#define SCB_STATUS   0x00   /* 16-bit: status word; ack byte is the high half */
#define SCB_CMD      0x02   /* 16-bit: command byte + interrupt mask byte     */
#define SCB_POINTER  0x04   /* 32-bit: the general pointer                    */
#define SCB_PORT     0x08   /* 32-bit: reset and self-test                    */
#define SCB_EEPROM   0x0E   /* 16-bit: the three-wire EEPROM, bit by bit      */

/* Command-unit commands, in bits 6:4 of the command byte. */
#define CU_START     0x10
#define CU_RESUME    0x20
#define CU_BASE      0x60   /* load the CU base address                       */
/* Receive-unit commands, in bits 2:0. */
#define RU_START     0x01
#define RU_RESUME    0x02
#define RU_BASE      0x06   /* load the RU base address                       */

/* Command block opcodes, in bits 2:0 of the command word. */
#define CB_NOP       0x0000
#define CB_IA_SETUP  0x0001
#define CB_CONFIGURE 0x0002
#define CB_TRANSMIT  0x0004
#define CB_EL        0x8000  /* end of list: the CU stops after this one      */
#define CB_S         0x4000  /* suspend after this one                        */
#define CB_STATUS_C  0x8000  /* the card sets this when the block is done     */
#define CB_STATUS_OK 0x2000

/* Receive frame descriptor bits. */
#define RFD_EL       0x8000  /* in the SIZE field                             */
#define RFD_C        0x8000  /* in the STATUS field: this one is filled        */
#define RFD_EOF      0x8000  /* in the COUNT field                            */
#define RFD_COUNT_MASK 0x3FFF

/* EEPROM control bits in SCB_EEPROM. */
#define EE_SK 0x01
#define EE_CS 0x02
#define EE_DI 0x04
#define EE_DO 0x08

#define I8255X_RX_DESCS 16
#define I8255X_TX_DESCS 8
#define I8255X_BUF_SZ   1536

/* A transmit command block in SIMPLIFIED MODE: the frame data sits directly
 * after the header instead of being described by a buffer list. That costs a
 * copy and removes the entire scatter-list path, which on a card this old is
 * the right trade. */
struct tx_cb {
    volatile uint16_t status;
    uint16_t command;
    uint32_t link;            /* physical address of the next block */
    uint32_t tbd_addr;        /* 0xFFFFFFFF selects simplified mode */
    uint16_t count;           /* byte count, with the EOF bit set   */
    uint8_t  threshold;
    uint8_t  tbd_count;
    uint8_t  data[I8255X_BUF_SZ];
} __attribute__((packed));

struct rfd {
    volatile uint16_t status;
    uint16_t command;
    uint32_t link;
    uint32_t rbd_addr;        /* 0xFFFFFFFF: the data follows the header */
    volatile uint16_t count;
    uint16_t size;
    uint8_t  data[I8255X_BUF_SZ];
} __attribute__((packed));

/* A one-off command block used for setup (station address, configuration). */
struct setup_cb {
    volatile uint16_t status;
    uint16_t command;
    uint32_t link;
    uint8_t  data[32];
} __attribute__((packed));

static struct tx_cb  g_tx[I8255X_TX_DESCS] __attribute__((aligned(16)));
static struct rfd    g_rx[I8255X_RX_DESCS] __attribute__((aligned(16)));
static struct setup_cb g_setup __attribute__((aligned(16)));

static volatile uint8_t *g_mmio;
static uint16_t g_io;              /* used when the BAR is I/O rather than MMIO */
static bool g_use_mmio;
static uint32_t g_tx_next, g_rx_next, g_rx_last;
static spinlock_t g_tx_lock = SPINLOCK_INIT;
static int g_present;
static uint8_t g_mac[ETH_ALEN];

static inline uint32_t dma32(const volatile void *p) {
    return (uint32_t)KV2P((uint64_t)(uintptr_t)p);
}
#if defined(__x86_64__)
static inline uint8_t  cr8 (uint32_t o) { return g_use_mmio ? *(volatile uint8_t  *)(g_mmio+o) : inb ((uint16_t)(g_io+o)); }
static inline uint16_t cr16(uint32_t o) { return g_use_mmio ? *(volatile uint16_t *)(g_mmio+o) : inw ((uint16_t)(g_io+o)); }
static inline void cw8 (uint32_t o, uint8_t v)  { if (g_use_mmio) *(volatile uint8_t  *)(g_mmio+o)=v; else outb((uint16_t)(g_io+o), v); }
static inline void cw16(uint32_t o, uint16_t v) { if (g_use_mmio) *(volatile uint16_t *)(g_mmio+o)=v; else outw((uint16_t)(g_io+o), v); }
static inline void cw32(uint32_t o, uint32_t v) { if (g_use_mmio) *(volatile uint32_t *)(g_mmio+o)=v; else outl((uint16_t)(g_io+o), v); }
#else
static inline uint8_t  cr8 (uint32_t o) { return *(volatile uint8_t  *)(g_mmio+o); }
static inline uint16_t cr16(uint32_t o) { return *(volatile uint16_t *)(g_mmio+o); }
static inline void cw8 (uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(g_mmio+o)=v; }
static inline void cw16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(g_mmio+o)=v; }
static inline void cw32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(g_mmio+o)=v; }
#endif

static void tiny_delay(void) { for (volatile int i = 0; i < 200; i++) { } }

/* THE COMMAND BYTE MUST BE ZERO BEFORE A NEW COMMAND. The card clears it when
 * it has accepted the previous one; writing over a non-zero byte loses the
 * command silently. */
static bool scb_wait(void) {
    for (int i = 0; i < 1000000; i++) {
        if (cr8(SCB_CMD) == 0) return true;
        arch_cpu_relax();
    }
    return false;
}

static bool scb_command(uint8_t cmd, uint32_t ptr) {
    if (!scb_wait()) return false;
    cw32(SCB_POINTER, ptr);
    cw8(SCB_CMD, cmd);
    return true;
}

/* ---- the three-wire EEPROM ---------------------------------------------
 *
 * A 93C46: 64 words of 16 bits, addressed by clocking a start bit, a two-bit
 * opcode and six address bits in on DI, then clocking sixteen bits of the
 * answer out on DO. One wire each way and one clock, bit-banged through the
 * bottom four bits of a single register.
 *
 * THE COMMAND AND THE ANSWER SHARE ONE SHIFT LOOP. The data does not start
 * arriving when the command ends -- the chip clocks it out on the SAME clock
 * edges, sixteen more of them, so the loop keeps running with zeros on DI and
 * keeps shifting DO into the result. Stopping when the command bits run out
 * yields ten bits of nothing, which is what "a MAC address of 29:2a:1a:2b" is
 * when you see one. That was this driver's first reading.
 *
 * The address width is six bits on every part that matters and eight on a few
 * later ones. There is no register that says which; the accepted way to tell
 * is to read word zero at six and see whether the chip answers at all. */
#define EE_ENB   (0x4800 | EE_CS)         /* chip select, clock low   */

static void ee_write(uint16_t v) { cw16(SCB_EEPROM, v); tiny_delay(); }

static uint16_t ee_read_word(int addr, int addr_len) {
    /* The command sits in the TOP of the shift value and sixteen zeros follow
     * it; `bits` counts every clock, command and answer together. */
    uint32_t cmd = (uint32_t)(((6u << addr_len) | (uint32_t)addr) << 16);
    int bits = 3 + addr_len + 16;
    uint32_t retval = 0;

    ee_write(EE_ENB | EE_SK);
    do {
        uint16_t dataval = (cmd & (1u << bits)) ? (uint16_t)(EE_ENB | EE_DI)
                                                : (uint16_t)EE_ENB;
        ee_write(dataval);
        ee_write((uint16_t)(dataval | EE_SK));
        retval = (retval << 1) | ((cr16(SCB_EEPROM) & EE_DO) ? 1u : 0u);
    } while (--bits >= 0);

    ee_write(EE_ENB);
    ee_write(EE_ENB & (uint16_t)~EE_CS);   /* release the chip */
    return (uint16_t)(retval & 0xFFFFu);
}

static int ee_addr_bits(void) {
    /* An unanswered read floats DO high and reads back as all ones. */
    return ee_read_word(0, 6) == 0xFFFF ? 8 : 6;
}

/* ---- bring-up ----------------------------------------------------------- */

static bool run_setup_cb(uint16_t opcode, const uint8_t *data, uint32_t len) {
    memset(&g_setup, 0, sizeof g_setup);
    g_setup.command = (uint16_t)(opcode | CB_EL);
    g_setup.link = 0xFFFFFFFFu;
    if (data && len) memcpy(g_setup.data, data, len);
    __sync_synchronize();
    if (!scb_command(CU_START, dma32(&g_setup))) return false;
    for (int i = 0; i < 2000000; i++) {
        if (g_setup.status & CB_STATUS_C) return (g_setup.status & CB_STATUS_OK) != 0;
        arch_cpu_relax();
    }
    return false;
}

static void rx_ring_init(void) {
    for (int i = 0; i < I8255X_RX_DESCS; i++) {
        memset(&g_rx[i], 0, sizeof g_rx[i]);
        g_rx[i].link = dma32(&g_rx[(i + 1) % I8255X_RX_DESCS]);
        g_rx[i].rbd_addr = 0xFFFFFFFFu;
        g_rx[i].size = I8255X_BUF_SZ;
    }
    /* THE LAST ONE ENDS THE LIST. Without it the receive unit walks the ring
     * forever and overwrites frames the driver has not read. */
    g_rx[I8255X_RX_DESCS - 1].size |= RFD_EL;
    g_rx_next = 0;
    g_rx_last = I8255X_RX_DESCS - 1;
}

static void tx_ring_init(void) {
    for (int i = 0; i < I8255X_TX_DESCS; i++) {
        memset(&g_tx[i], 0, sizeof g_tx[i]);
        g_tx[i].status = CB_STATUS_C;      /* free */
        g_tx[i].tbd_addr = 0xFFFFFFFFu;
        g_tx[i].tbd_count = 0;
        g_tx[i].threshold = 0xE0;
    }
    g_tx_next = 0;
}

/* The PRO/100 family's device ids. A list, not a class match, because every
 * Intel ethernet part shares vendor 0x8086 and class 02:00 -- and e1000.c
 * matches on exactly that pair. Exported so that driver can skip these, in
 * ONE place, rather than the two lists drifting apart.
 *
 * This is not hypothetical. Before it existed, e1000.c claimed an 82559ER,
 * mapped its 4 KiB register window, and read the station address at offset
 * 0x5400 -- twenty times past the end of the mapping. The result was a
 * kernel page fault during boot on any machine with a PRO/100 in it. */
bool i8255x_owns_device(uint16_t device_id) {
    switch (device_id) {
    case 0x1029: case 0x1030: case 0x1031: case 0x1032: case 0x1033:
    case 0x1034: case 0x1038: case 0x1039: case 0x103A: case 0x103B:
    case 0x103C: case 0x103D: case 0x103E:
    case 0x1050: case 0x1051: case 0x1059:
    case 0x1209: case 0x1227: case 0x1228: case 0x1229:
    case 0x2449: case 0x2459: case 0x245D: case 0x27DC:
        return true;
    default:
        return false;
    }
}

static const struct pci_device *i8255x_find(void) {
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->vendor_id != 0x8086) continue;
        if (d->class_code != 0x02 || d->subclass != 0x00) continue;
        if (i8255x_owns_device(d->device_id)) return d;
    }
    return 0;
}

bool i8255x_init(uint8_t mac_out[ETH_ALEN]) {
    const struct pci_device *d = i8255x_find();
    if (!d) return false;

    /* BAR0 is the card's memory-mapped CSR and BAR1 the same registers in I/O
     * space. Either works; the MMIO one is preferred because an I/O access is
     * a VM exit on an emulator and a bus cycle on real hardware. */
    struct pci_bar b0 = pci_read_bar(d->bus, d->device, d->function, 0);
    struct pci_bar b1 = pci_read_bar(d->bus, d->device, d->function, 1);
    if (b0.valid && b0.is_mmio) {
        g_mmio = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(b0.address,
                                                            b0.size ? b0.size : 0x1000);
        g_use_mmio = g_mmio != 0;
    }
    if (!g_use_mmio) {
#if defined(__x86_64__)
        if (!b1.valid || b1.is_mmio) {
            kprintf("i8255x: %04x:%04x has neither an MMIO nor an I/O CSR\n",
                    d->vendor_id, d->device_id);
            return false;
        }
        g_io = (uint16_t)b1.address;
#else
        (void)b1;
        kprintf("i8255x: %04x:%04x has no memory-mapped CSR, and this "
                "machine has no I/O port space to fall back on\n",
                d->vendor_id, d->device_id);
        return false;
#endif
    }
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    /* A software reset through the PORT register. Value 0 is "selective
     * reset"; the card needs a moment afterwards before it answers. */
    cw32(SCB_PORT, 0);
    for (volatile int i = 0; i < 400000; i++) { }
    cw8(SCB_CMD + 1, 0xFF);                 /* mask every interrupt: we poll */

    int abits = ee_addr_bits();
    for (int w = 0; w < 3; w++) {
        uint16_t v = ee_read_word(w, abits);
        g_mac[w * 2]     = (uint8_t)v;
        g_mac[w * 2 + 1] = (uint8_t)(v >> 8);
    }
    /* An all-zero or all-ones address means the EEPROM did not answer, and a
     * card with no address must not pretend to have one. */
    bool zero = true, ones = true;
    for (int i = 0; i < ETH_ALEN; i++) {
        if (g_mac[i] != 0x00) zero = false;
        if (g_mac[i] != 0xFF) ones = false;
    }
    if (zero || ones) {
        kprintf("i8255x: EEPROM gave no station address\n");
        return false;
    }
    memcpy(mac_out, g_mac, ETH_ALEN);

    /* Both units are told their base address is zero, which means every
     * pointer in every block is an absolute physical address. The alternative
     * is offsets from a base, which buys nothing here. */
    if (!scb_command(CU_BASE, 0) || !scb_wait()) return false;
    if (!scb_command(RU_BASE, 0) || !scb_wait()) return false;

    /* CONFIGURE: 22 bytes whose defaults are mostly right. The bytes that are
     * set explicitly are the ones whose reset value is wrong for a driver
     * that wants ordinary Ethernet. */
    uint8_t cfg[22] = {
        22,          /* byte count                                        */
        0x08,        /* Tx/Rx FIFO limits                                 */
        0, 0,
        0,           /* no Rx DMA byte count                              */
        0x80,        /* Tx DMA max byte count enable                      */
        0x32,        /* standard statistical counters, save bad frames off*/
        0x03,        /* discard short receives, underrun retry            */
        0x01,        /* MII                                               */
        0, 0x2E, 0,
        0x60,        /* default interframe spacing                        */
        0, 0xF2,
        0xC8,        /* padding enabled, strip CRC                        */
        0, 0x40,
        0xF2,        /* BROADCAST ENABLED -- clearing this bit is what
                      * makes DHCP silently fail on this card             */
        0x80, 0x3F, 0x05,
    };
    if (!run_setup_cb(CB_CONFIGURE, cfg, sizeof cfg)) {
        kprintf("i8255x: CONFIGURE command was not accepted\n");
        return false;
    }
    if (!run_setup_cb(CB_IA_SETUP, g_mac, ETH_ALEN)) {
        kprintf("i8255x: station address was not accepted\n");
        return false;
    }

    rx_ring_init();
    tx_ring_init();
    __sync_synchronize();
    if (!scb_command(RU_START, dma32(&g_rx[0]))) return false;

    kprintf("i8255x: %04x:%04x up, MAC %02x:%02x:%02x:%02x:%02x:%02x (%s CSR)\n",
            d->vendor_id, d->device_id,
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5],
            g_use_mmio ? "memory" : "I/O");
    g_present = 1;
    return true;
}

int i8255x_tx(const void *frame, uint32_t len) {
    if (!g_present || !frame || !len || len > I8255X_BUF_SZ) return -1;
    spin_lock(&g_tx_lock);
    uint32_t i = g_tx_next;
    struct tx_cb *cb = &g_tx[i];
    if (!(cb->status & CB_STATUS_C)) { spin_unlock(&g_tx_lock); return -1; }

    memcpy(cb->data, frame, len);
    cb->status   = 0;
    cb->command  = CB_TRANSMIT | CB_EL;
    cb->link     = 0xFFFFFFFFu;
    cb->tbd_addr = 0xFFFFFFFFu;
    cb->count    = (uint16_t)(len | RFD_EOF);
    cb->tbd_count = 0;
    cb->threshold = 0xE0;
    __sync_synchronize();

    /* Each frame is its own one-block list, started fresh. Chaining onto a
     * running list needs CU_RESUME and a race-free way to append, which is
     * worth doing when this card is a bottleneck and not before. */
    if (!scb_command(CU_START, dma32(cb))) { spin_unlock(&g_tx_lock); return -1; }

    for (int spin = 0; spin < 2000000 && !(cb->status & CB_STATUS_C); spin++)
        arch_cpu_relax();

    int ok = (cb->status & CB_STATUS_C) ? (int)len : -1;
    g_tx_next = (i + 1) % I8255X_TX_DESCS;
    spin_unlock(&g_tx_lock);
    return ok;
}

int i8255x_link(void) {
    /* The link state lives in the PHY, behind the MDI register, and reading
     * it is a two-step handshake this driver does not do. Saying "I cannot
     * tell" is the honest answer and net.c treats it as up. */
    return g_present ? -1 : -1;
}

void i8255x_poll(void) {
    if (!g_present) return;
    bool any = false;

    while (g_rx[g_rx_next].status & RFD_C) {
        struct rfd *r = &g_rx[g_rx_next];
        uint16_t count = r->count & RFD_COUNT_MASK;
        if (count >= ETH_HLEN && count <= I8255X_BUF_SZ)
            net_rx(r->data, count);

        /* Hand it back and MOVE THE END OF THE LIST onto it, so the receive
         * unit always has somewhere to stop that is behind us rather than in
         * front. Doing this in the other order -- clear the old end first --
         * leaves a window where the list has no end at all. */
        r->status = 0;
        r->count = 0;
        r->size = I8255X_BUF_SZ | RFD_EL;
        __sync_synchronize();
        g_rx[g_rx_last].size = I8255X_BUF_SZ;
        g_rx_last = g_rx_next;
        g_rx_next = (g_rx_next + 1) % I8255X_RX_DESCS;
        any = true;
    }

    /* THE RECEIVE UNIT STOPS AND STAYS STOPPED. Bits 5:2 of the status word
     * are its state; 0 is idle and 2 is no-resources. Either means it has
     * given up and will not take another frame until it is told to resume.
     * This is the bug that makes an eepro100 driver work for exactly as many
     * packets as it has descriptors. */
    uint16_t st = cr16(SCB_STATUS);
    uint8_t ru_state = (uint8_t)((st >> 2) & 0x0F);
    if (any || ru_state == 0 || ru_state == 2) {
        cw8(SCB_STATUS + 1, (uint8_t)(st >> 8));   /* acknowledge everything */
        if (ru_state != 4)                          /* 4 = ready */
            scb_command(RU_RESUME, 0);
    }
}
