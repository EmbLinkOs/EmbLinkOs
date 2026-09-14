/* kernel/drivers/storage/esp.c -- the NCR/AMD 53C9x "ESP", as am53c974.
 *
 * The oldest thing in this directory by twenty years, and it shows: there is
 * no descriptor, no ring and no message. There is a SIXTEEN-BYTE FIFO and a
 * command register, and the driver walks the SCSI bus phases by hand --
 * select, command, data, status, message -- reading the chip after each one
 * to find out which phase it is now in.
 *
 * THE PHASE IS THE PROGRAM COUNTER. Everything this driver does is decided by
 * the bottom three bits of the status register, and those bits are cleared by
 * reading the interrupt register. So the status must be read FIRST and the
 * interrupt register second, every single time; the other order loses the
 * phase and the driver has no idea what the bus is doing.
 *
 * DATA MOVES THROUGH A SEPARATE DMA ENGINE that is not part of the SCSI chip
 * at all -- it belongs to the PCI wrapper AMD put around it, and it is
 * programmed with its own address and count registers in a different part of
 * the I/O window. The chip's own transfer counter has to be set to the same
 * number, in three eight-bit registers, or the two disagree about when the
 * transfer is over.
 *
 * The commands are ordinary SCSI from kernel/block/scsi.c, like every other
 * controller here.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#if defined(__x86_64__)
#include "include/io.h"
#endif
#include "drivers/bus/pci.h"
#include "drivers/storage/esp.h"
#include "block/block.h"
#include "block/scsi.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

/* Core registers. Each occupies FOUR bytes of the I/O window even though it
 * is one byte wide -- the PCI wrapper spaces them out. */
#define ESP_TCLO   0x0
#define ESP_TCMID  0x1
#define ESP_FIFO   0x2
#define ESP_CMD    0x3
#define ESP_STAT   0x4   /* read */
#define ESP_BUSID  0x4   /* write */
#define ESP_INTR   0x5   /* read */
#define ESP_SEQ    0x6
#define ESP_FFLAGS 0x7
#define ESP_CFG1   0x8
#define ESP_CFG2   0xB
#define ESP_TCHI   0xE

#define CMD_DMA      0x80
#define CMD_NOP      0x00
#define CMD_FLUSH    0x01
#define CMD_RESET    0x02
#define CMD_BUSRESET 0x03
#define CMD_TI       0x10   /* transfer information: move the data */
#define CMD_ICCS     0x11   /* initiator command complete: fetch status  */
#define CMD_MSGACC   0x12   /* accept the message and free the bus       */
#define CMD_SELATN   0x42   /* select the target, with attention         */

#define STAT_PHASE  0x07
#define PHASE_DO    0x00    /* data out  */
#define PHASE_DI    0x01    /* data in   */
#define PHASE_CMD   0x02
#define PHASE_ST    0x03    /* status    */
#define PHASE_MI    0x07    /* message in */
#define STAT_TC     0x10
#define STAT_INT    0x80

#define INTR_FC  0x08   /* function complete  */
#define INTR_BS  0x10   /* bus service        */
#define INTR_DC  0x20   /* disconnected       */
#define INTR_RST 0x80

/* The PCI wrapper's DMA engine, at 0x40 in the same window. */
#define DMA_CMD   0x40
#define DMA_STC   0x44
#define DMA_SPA   0x48
#define DMA_STAT  0x54

#define DMA_CMD_START 0x03
#define DMA_CMD_DIR   0x80   /* set: the engine writes memory (a read)      */

#define ESP_MAX_TARGETS 8
#define ESP_XFER        16384

static uint16_t g_io;
static bool g_up;
static uint8_t g_data[ESP_XFER] __attribute__((aligned(64)));

struct esp_target {
    bool used;
    uint8_t target;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct esp_target g_targets[ESP_MAX_TARGETS];

#if defined(__x86_64__)
/* EVERY ACCESS IS 32 BITS WIDE. The wrapper decodes four-byte accesses and
 * reconstructs anything narrower by read-modify-write, which for a register
 * whose read and write meanings differ -- and half of these registers have
 * two completely different meanings -- is not the same operation. */
static inline uint8_t rr(uint32_t reg)  { return (uint8_t)inl((uint16_t)(g_io + reg * 4)); }
static inline void    rw(uint32_t reg, uint8_t v) { outl((uint16_t)(g_io + reg * 4), v); }
static inline uint32_t dr(uint32_t off) { return inl((uint16_t)(g_io + off)); }
static inline void     dw(uint32_t off, uint32_t v) { outl((uint16_t)(g_io + off), v); }
#else
static inline uint8_t rr(uint32_t reg) { (void)reg; return 0; }
static inline void    rw(uint32_t reg, uint8_t v) { (void)reg; (void)v; }
static inline uint32_t dr(uint32_t off) { (void)off; return 0; }
static inline void     dw(uint32_t off, uint32_t v) { (void)off; (void)v; }
#endif

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* Wait for the chip to want attention, then read the status and the interrupt
 * register IN THAT ORDER -- see the file comment. Returns false on timeout.
 * `*phase` is the bus phase the chip is in now; `*intr` what just happened. */
static bool esp_wait(uint8_t *phase, uint8_t *intr) {
    for (int i = 0; i < 10000000; i++) {
        uint8_t st = rr(ESP_STAT);
        if (st & STAT_INT) {
            *phase = st & STAT_PHASE;
            *intr = rr(ESP_INTR);      /* this clears the phase bits */
            return true;
        }
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

static void esp_set_count(uint32_t n) {
    rw(ESP_TCLO,  (uint8_t)n);
    rw(ESP_TCMID, (uint8_t)(n >> 8));
    rw(ESP_TCHI,  (uint8_t)(n >> 16));
}

static bool esp_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                     void *data, uint32_t len, bool to_dev) {
    struct esp_target *t = ctx;
    if (!g_up || len > ESP_XFER) return false;

    if (to_dev && data && len) memcpy(g_data, data, len);

    /* SELECTION. The FIFO is loaded with an IDENTIFY message and then the
     * command, and the chip sends both: the message tells the target which
     * logical unit and that this initiator can be disconnected from, and the
     * CDB follows in the command phase. Selecting WITHOUT the message works
     * on some targets and hangs on others, so it is always sent. */
    rw(ESP_CMD, CMD_FLUSH);
    rw(ESP_FIFO, 0xC0);                       /* IDENTIFY, LUN 0, disconnect ok */
    for (uint8_t i = 0; i < cdb_len; i++) rw(ESP_FIFO, cdb[i]);
    rw(ESP_BUSID, t->target);
    rw(ESP_CMD, CMD_SELATN);

    uint8_t phase = 0, intr = 0;
    if (!esp_wait(&phase, &intr)) {
        kprintf("esp: target %u never answered selection\n", t->target);
        return false;
    }
    /* DISCONNECTED means there is nothing at that address. That is the answer
     * to a probe, not a fault. */
    if (intr & INTR_DC) return false;

    /* DATA. The chip reports the phase it moved to; if the target wants data
     * this is where it says so. */
    if (len && (phase == PHASE_DI || phase == PHASE_DO)) {
        esp_set_count(len);
        dw(DMA_STC, len);
        dw(DMA_SPA, (uint32_t)dma(g_data));
        /* Direction is the ENGINE's, not the command's: set means the engine
         * writes into memory, which is a read from the device. */
        dw(DMA_CMD, DMA_CMD_START | (to_dev ? 0 : DMA_CMD_DIR));
        rw(ESP_CMD, CMD_TI | CMD_DMA);
        if (!esp_wait(&phase, &intr)) {
            kprintf("esp: target %u stalled during data\n", t->target);
            return false;
        }
        if (intr & INTR_DC) return false;
    }

    /* STATUS AND MESSAGE, fetched together. One command moves both into the
     * FIFO; the first byte out is the SCSI status and the second is the
     * command-complete message. */
    if (phase != PHASE_ST && phase != PHASE_MI) {
        /* Some targets need a nudge through an unexpected phase; ask again
         * rather than guessing what it holds. */
        rw(ESP_CMD, CMD_NOP);
    }
    rw(ESP_CMD, CMD_ICCS);
    if (!esp_wait(&phase, &intr)) {
        kprintf("esp: target %u never returned status\n", t->target);
        return false;
    }
    uint8_t status = rr(ESP_FIFO);
    (void)rr(ESP_FIFO);                        /* the message byte */

    /* Release the bus. Without this the target stays selected and the next
     * command is issued into a bus that is already busy. */
    rw(ESP_CMD, CMD_MSGACC);
    esp_wait(&phase, &intr);

    if (status != 0) return false;
    if (!to_dev && data && len) memcpy(data, g_data, len);
    return true;
}

static int esp_read(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, void *buf) {
    struct esp_target *t = b->driver_data;
    return scsi_read(&t->sd, lba, count, buf, ESP_XFER / t->sd.block_size);
}
static int esp_write(struct embk_block_device *b, uint64_t lba,
                     uint32_t count, const void *buf) {
    struct esp_target *t = b->driver_data;
    return scsi_write(&t->sd, lba, count, buf, ESP_XFER / t->sd.block_size);
}

bool esp_init(void) {
#if !defined(__x86_64__)
    return false;     /* the register window is I/O space, which ARM has none of */
#else
    if (g_up) return true;
    const struct pci_device *d = NULL;
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *c = pci_get_device(i);
        /* AMD Am53C974 / PCscsi, and the Tekram DC-390 built on it. */
        if (c && c->vendor_id == 0x1022 && c->device_id == 0x2020) { d = c; break; }
    }
    if (!d) return false;

    struct pci_bar bar = pci_read_bar(d->bus, d->device, d->function, 0);
    if (!bar.valid || bar.is_mmio) {
        kprintf("esp: BAR0 is not an I/O window\n");
        return false;
    }
    g_io = (uint16_t)bar.address;
    pci_enable_bus_mastering(d->bus, d->device, d->function);

    rw(ESP_CMD, CMD_RESET);
    for (volatile int i = 0; i < 100000; i++) { }
    rw(ESP_CMD, CMD_NOP);
    (void)rr(ESP_INTR);                /* clear whatever the reset raised */
    /* Configuration 1 holds THIS ADAPTER'S OWN SCSI ID, which is 7 by
     * convention -- the highest, because on a real bus the highest id wins
     * arbitration and the initiator should. */
    rw(ESP_CFG1, 0x07);
    rw(ESP_CFG2, 0x00);
    dw(DMA_CMD, 0);
    g_up = true;

    int found = 0;
    for (uint32_t tn = 0; tn < ESP_MAX_TARGETS; tn++) {
        if (tn == 7) continue;         /* that is us */
        struct esp_target *t = &g_targets[tn];
        memset(t, 0, sizeof *t);
        t->used = true;
        t->target = (uint8_t)tn;
        t->sd.exec = esp_exec;
        t->sd.ctx = t;
        if (!scsi_probe(&t->sd)) { t->used = false; continue; }

        t->blk.block_count = t->sd.blocks;
        t->blk.block_size  = t->sd.block_size;
        t->blk.read  = esp_read;
        t->blk.write = esp_write;
        t->blk.driver_data = t;
        /* THE DMA ENGINE'S ADDRESS REGISTER IS THIRTY-TWO BITS. Anything it
         * is asked to reach above four gigabytes is silently truncated, so
         * the block layer is told the limit rather than finding out. */
        t->blk.dma_max_phys = 0xFFFFFFFFULL;
        t->blk.needs_kernel_range = true;
        if (embk_block_register(&t->blk) != EMBK_OK) { t->used = false; continue; }
        kprintf("esp: %s = target %u, %s %s, %llu x %u B\n", t->blk.name, tn,
                t->sd.vendor, t->sd.product,
                (unsigned long long)t->sd.blocks, t->sd.block_size);
        found++;
    }
    if (!found) kprintf("esp: controller up, no targets answered\n");
    return true;
#endif
}

bool esp_present(void) { return g_up; }
uint32_t esp_target_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < ESP_MAX_TARGETS; i++) if (g_targets[i].used) n++;
    return n;
}
