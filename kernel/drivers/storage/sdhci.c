/* kernel/drivers/storage/sdhci.c -- the SD/eMMC host controller.
 *
 * This is the card reader in the side of a laptop, and on a great many ARM
 * boards it is also where the machine BOOTS FROM. It is the first storage
 * controller in this kernel that does not speak SCSI or ATA: an SD card is
 * commanded with numbered commands and answers with a bit-packed register,
 * and the host controller is a state machine you feed one command at a time.
 *
 * THE CARD IS NOT ADDRESSED THE SAME WAY DEPENDING ON ITS SIZE, and this is
 * the single most common way to get an SD driver wrong. A standard-capacity
 * card (<= 2 GB) takes a BYTE offset in the command argument. A high-capacity
 * card (SDHC/SDXC, and every eMMC part) takes a BLOCK number. Sending a byte
 * offset to an SDHC card reads block 0 of every request that fits in 512
 * bytes, and sending a block number to an SDSC card reads 512 bytes from 512
 * times too far in. Which one it is comes back in the OCR during
 * initialisation and is recorded as `block_addressed`.
 *
 * Transfers here are PIO through the buffer data port, not DMA. That is slow
 * -- 128 32-bit reads per block -- and it is deliberate: it removes the SDMA
 * boundary rules and the ADMA descriptor table from the first version of a
 * driver whose correctness is entirely in the command sequence. The DMA path
 * is a later change to this file and to nothing else.
 *
 * eMMC differs from SD in exactly two places, both marked below: it is woken
 * with CMD1 instead of ACMD41, and it is TOLD its relative address by CMD3
 * instead of being asked for one. Everything after that is shared.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/sdhci.h"
#include "block/block.h"
#include "mm/vmm.h"


/* Register offsets, SD Host Controller Simplified Specification 3.00 §2.2. */
#define SD_ARG2          0x00
#define SD_BLOCK_SIZE    0x04   /* 16-bit; block count at +2                */
#define SD_ARG1          0x08
#define SD_XFER_MODE     0x0C   /* 16-bit; command at +2                    */
#define SD_RESP0         0x10
#define SD_BUFFER        0x20
#define SD_PRESENT       0x24
#define SD_HOST_CTL      0x28   /* 8-bit; power +1, block gap +2, wake +3   */
#define SD_CLOCK_CTL     0x2C   /* 16-bit; timeout +2, soft reset +3        */
#define SD_INT_STATUS    0x30   /* normal 16-bit; error status at +2        */
#define SD_INT_ENABLE    0x34
#define SD_SIGNAL_ENABLE 0x38
#define SD_CAPS          0x40
#define SD_VERSION       0xFE

#define PRESENT_CMD_INHIBIT   (1u << 0)
#define PRESENT_DAT_INHIBIT   (1u << 1)
#define PRESENT_BUF_WRITE_EN  (1u << 10)
#define PRESENT_BUF_READ_EN   (1u << 11)
#define PRESENT_CARD_INSERTED (1u << 16)

#define INT_CMD_COMPLETE   (1u << 0)
#define INT_XFER_COMPLETE  (1u << 1)
#define INT_BUF_WRITE_RDY  (1u << 4)
#define INT_BUF_READ_RDY   (1u << 5)
#define INT_ERROR          (1u << 15)

#define XFER_BLOCK_COUNT_EN (1u << 1)
#define XFER_DIR_READ       (1u << 4)
#define XFER_MULTI_BLOCK    (1u << 5)

/* Command register: index in bits 13:8, then the flags below. */
#define CMD_RESP_NONE   0u
#define CMD_RESP_136    1u
#define CMD_RESP_48     2u
#define CMD_RESP_48_BSY 3u
#define CMD_CRC_CHECK   (1u << 3)
#define CMD_INDEX_CHECK (1u << 4)
#define CMD_DATA        (1u << 5)

/* SD/MMC command indices, named. */
#define CMD_GO_IDLE          0
#define CMD_SEND_OP_COND     1    /* eMMC only */
#define CMD_ALL_SEND_CID     2
#define CMD_SEND_RELATIVE    3
#define CMD_SELECT           7
#define CMD_SEND_IF_COND     8
#define CMD_SEND_CSD         9
#define CMD_SET_BLOCKLEN    16
#define CMD_READ_SINGLE     17
#define CMD_READ_MULTI      18
#define CMD_WRITE_SINGLE    24
#define CMD_WRITE_MULTI     25
#define CMD_APP_CMD         55
#define ACMD_SD_SEND_OP_COND 41

#define SDHCI_MAX_HOSTS 2
#define SD_BLOCK_BYTES  512u

struct sdhci_host {
    bool used;
    volatile uint8_t *regs;
    uint16_t rca;               /* relative card address, in bits 31:16 of args */
    bool block_addressed;       /* SDHC/SDXC/eMMC: the argument is a block number */
    bool is_emmc;
    uint64_t blocks;
    struct embk_block_device blk;
};
static struct sdhci_host g_hosts[SDHCI_MAX_HOSTS];
static uint32_t g_host_count;

static inline uint8_t  r8 (struct sdhci_host *h, uint32_t o) { return *(volatile uint8_t  *)(h->regs + o); }
static inline uint16_t r16(struct sdhci_host *h, uint32_t o) { return *(volatile uint16_t *)(h->regs + o); }
static inline uint32_t r32(struct sdhci_host *h, uint32_t o) { return *(volatile uint32_t *)(h->regs + o); }
static inline void w8 (struct sdhci_host *h, uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(h->regs + o) = v; }
static inline void w16(struct sdhci_host *h, uint32_t o, uint16_t v) { *(volatile uint16_t *)(h->regs + o) = v; }
static inline void w32(struct sdhci_host *h, uint32_t o, uint32_t v) { *(volatile uint32_t *)(h->regs + o) = v; }

static bool wait_bit_clear(struct sdhci_host *h, uint32_t reg, uint32_t mask) {
    for (int i = 0; i < 1000000; i++) {
        if (!(r32(h, reg) & mask)) return true;
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* Issue one command and wait for Command Complete. `resp_type` is one of the
 * CMD_RESP_* values; `data` says whether a data transfer follows, which the
 * controller needs told BEFORE the command goes out.
 *
 * BOTH INHIBIT BITS MUST BE CLEAR FIRST, and the second one is the one people
 * forget: CMD inhibit says the command line is free, DAT inhibit says the
 * previous transfer has finished with the data lines. Issuing a read while
 * DAT is still busy is accepted by the controller and produces the previous
 * block's data. */
static bool sd_cmd(struct sdhci_host *h, uint8_t index, uint32_t arg,
                   uint32_t resp_type, bool data, bool busy_resp) {
    uint32_t inhibit = PRESENT_CMD_INHIBIT | ((busy_resp || data) ? PRESENT_DAT_INHIBIT : 0);
    if (!wait_bit_clear(h, SD_PRESENT, inhibit)) {
        kprintf("sdhci: controller stuck busy before CMD%u\n", index);
        return false;
    }

    /* Clear the status bits we are about to wait on. A stale Command Complete
     * from the previous command makes the next one appear to finish
     * instantly, with whatever was in the response registers. */
    w32(h, SD_INT_STATUS, 0xFFFFFFFFu);
    w32(h, SD_ARG1, arg);

    uint16_t cmd = (uint16_t)(index << 8) | (uint16_t)resp_type;
    if (resp_type != CMD_RESP_NONE) cmd |= CMD_CRC_CHECK;
    /* R2 (136-bit) and R3 (the OCR) carry no command index to check against,
     * so index checking must be OFF for them or every one of them errors. */
    if (resp_type == CMD_RESP_48 || resp_type == CMD_RESP_48_BSY) cmd |= CMD_INDEX_CHECK;
    if (data) cmd |= CMD_DATA;
    w16(h, SD_XFER_MODE + 2, cmd);

    for (int i = 0; i < 1000000; i++) {
        uint32_t st = r32(h, SD_INT_STATUS);
        if (st & INT_ERROR) {
            w32(h, SD_INT_STATUS, 0xFFFFFFFFu);
            return false;
        }
        if (st & INT_CMD_COMPLETE) {
            w32(h, SD_INT_STATUS, INT_CMD_COMPLETE);
            return true;
        }
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

/* PIO one block in or out. The controller raises Buffer Read/Write Ready per
 * block, and the port is read 32 bits at a time regardless of bus width. */
static bool sd_pio_block(struct sdhci_host *h, uint8_t *buf, bool read) {
    uint32_t want = read ? INT_BUF_READ_RDY : INT_BUF_WRITE_RDY;
    for (int i = 0; i < 1000000; i++) {
        uint32_t st = r32(h, SD_INT_STATUS);
        if (st & INT_ERROR) return false;
        if (st & want) { w32(h, SD_INT_STATUS, want); goto ready; }
        __asm__ volatile("" ::: "memory");
    }
    return false;
ready:
    for (uint32_t i = 0; i < SD_BLOCK_BYTES / 4; i++) {
        if (read) {
            uint32_t v = r32(h, SD_BUFFER);
            buf[i*4+0] = (uint8_t)v;       buf[i*4+1] = (uint8_t)(v >> 8);
            buf[i*4+2] = (uint8_t)(v >> 16); buf[i*4+3] = (uint8_t)(v >> 24);
        } else {
            uint32_t v = (uint32_t)buf[i*4+0] | ((uint32_t)buf[i*4+1] << 8) |
                         ((uint32_t)buf[i*4+2] << 16) | ((uint32_t)buf[i*4+3] << 24);
            w32(h, SD_BUFFER, v);
        }
    }
    return true;
}

static bool sd_transfer(struct sdhci_host *h, uint64_t lba, uint32_t count,
                        void *buf, bool read) {
    if (!count) return true;
    uint8_t *p = buf;

    w16(h, SD_BLOCK_SIZE, (uint16_t)SD_BLOCK_BYTES);
    w16(h, SD_BLOCK_SIZE + 2, (uint16_t)count);

    uint16_t mode = XFER_BLOCK_COUNT_EN;
    if (read) mode |= XFER_DIR_READ;
    if (count > 1) mode |= XFER_MULTI_BLOCK;
    w16(h, SD_XFER_MODE, mode);

    /* THE ARGUMENT'S UNIT depends on the card, not on the controller. */
    uint32_t arg = h->block_addressed ? (uint32_t)lba
                                      : (uint32_t)(lba * SD_BLOCK_BYTES);
    uint8_t index = read ? (count > 1 ? CMD_READ_MULTI : CMD_READ_SINGLE)
                         : (count > 1 ? CMD_WRITE_MULTI : CMD_WRITE_SINGLE);
    if (!sd_cmd(h, index, arg, CMD_RESP_48, true, false)) return false;

    for (uint32_t b = 0; b < count; b++)
        if (!sd_pio_block(h, p + b * SD_BLOCK_BYTES, read)) return false;

    for (int i = 0; i < 5000000; i++) {
        uint32_t st = r32(h, SD_INT_STATUS);
        if (st & INT_ERROR) return false;
        if (st & INT_XFER_COMPLETE) { w32(h, SD_INT_STATUS, INT_XFER_COMPLETE); return true; }
        __asm__ volatile("" ::: "memory");
    }
    return false;
}

static int sd_blk_read(struct embk_block_device *d, uint64_t lba,
                       uint32_t count, void *buf) {
    struct sdhci_host *h = d->driver_data;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count > 64 ? 64 : count;   /* bounded so one stall is bounded */
        if (!sd_transfer(h, lba, n, p, true)) return -EMBK_EIO;
        lba += n; count -= n; p += n * SD_BLOCK_BYTES;
    }
    return EMBK_OK;
}
static int sd_blk_write(struct embk_block_device *d, uint64_t lba,
                        uint32_t count, const void *buf) {
    struct sdhci_host *h = d->driver_data;
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        if (!sd_transfer(h, lba, n, (void *)(uintptr_t)p, false)) return -EMBK_EIO;
        lba += n; count -= n; p += n * SD_BLOCK_BYTES;
    }
    return EMBK_OK;
}

/* Reset, power and clock. The order is fixed by the specification and none of
 * it is optional: a card clocked before it is powered never answers. */
static bool sd_host_reset(struct sdhci_host *h) {
    w8(h, SD_CLOCK_CTL + 3, 0x01);            /* software reset: all */
    for (int i = 0; i < 1000000; i++) {
        if (!(r8(h, SD_CLOCK_CTL + 3) & 0x01)) goto reset_done;
        __asm__ volatile("" ::: "memory");
    }
    kprintf("sdhci: controller did not come out of reset\n");
    return false;
reset_done:;

    /* Voltage: take the highest the controller says it supports. The bits are
     * capabilities 26 (1.8V), 25 (3.0V), 24 (3.3V). */
    uint32_t caps = r32(h, SD_CAPS);
    uint8_t power;
    if (caps & (1u << 24))      power = (7u << 1);   /* 3.3V */
    else if (caps & (1u << 25)) power = (6u << 1);   /* 3.0V */
    else if (caps & (1u << 26)) power = (5u << 1);   /* 1.8V */
    else { kprintf("sdhci: controller supports no voltage we can select\n"); return false; }
    w8(h, SD_HOST_CTL + 1, (uint8_t)(power | 1u));   /* select, then bus power on */

    /* Clock: the largest divider available, which is the slowest and is what
     * initialisation is supposed to use. Speed is a later negotiation and not
     * one this driver makes. */
    w16(h, SD_CLOCK_CTL, 0);
    w16(h, SD_CLOCK_CTL, (uint16_t)((0x80u << 8) | 0x01u));  /* div 256, internal enable */
    for (int i = 0; i < 1000000; i++) {
        if (r16(h, SD_CLOCK_CTL) & 0x02) goto clock_stable;  /* internal clock stable */
        __asm__ volatile("" ::: "memory");
    }
    kprintf("sdhci: internal clock never reported stable\n");
    return false;
clock_stable:
    w16(h, SD_CLOCK_CTL, (uint16_t)(r16(h, SD_CLOCK_CTL) | 0x04u));  /* SD clock on */
    w8(h, SD_CLOCK_CTL + 2, 0x0E);                                   /* timeout: max */

    /* Enable every status bit. Not the SIGNAL enables -- this driver polls,
     * and turning on signalling would route an interrupt nothing handles.
     * The distinction between those two registers is exactly that. */
    w32(h, SD_INT_ENABLE, 0xFFFFFFFFu);
    w32(h, SD_SIGNAL_ENABLE, 0);
    w32(h, SD_INT_STATUS, 0xFFFFFFFFu);
    return true;
}

/* Wake the card and learn how it is addressed. Returns false if nothing
 * answered. */
static bool sd_card_init(struct sdhci_host *h) {
    sd_cmd(h, CMD_GO_IDLE, 0, CMD_RESP_NONE, false, false);

    /* CMD8 asks "do you understand 2.0 voltages?" with a check pattern that
     * must come back unchanged. A card that ignores it is pre-2.0, which
     * means standard capacity and byte addressing. */
    bool v2 = sd_cmd(h, CMD_SEND_IF_COND, 0x000001AAu, CMD_RESP_48, false, false)
              && (r32(h, SD_RESP0) & 0xFFu) == 0xAAu;

    uint32_t ocr = 0;
    bool ready = false;
    for (int attempt = 0; attempt < 2000 && !ready; attempt++) {
        /* SD: CMD55 then ACMD41. eMMC: CMD1. The host does not know which it
         * has until one of them answers, so SD is tried first and eMMC is the
         * fallback -- an eMMC part does not implement CMD55 at all. */
        if (!h->is_emmc) {
            if (sd_cmd(h, CMD_APP_CMD, 0, CMD_RESP_48, false, false) &&
                sd_cmd(h, ACMD_SD_SEND_OP_COND,
                       (v2 ? 0x40000000u : 0u) | 0x00FF8000u,
                       CMD_RESP_48, false, false)) {
                ocr = r32(h, SD_RESP0);
                if (ocr & 0x80000000u) ready = true;
                continue;
            }
            if (attempt == 0) { h->is_emmc = true; continue; }   /* try eMMC */
            return false;
        }
        if (!sd_cmd(h, CMD_SEND_OP_COND, 0x40FF8000u, CMD_RESP_48, false, false))
            return false;
        ocr = r32(h, SD_RESP0);
        if (ocr & 0x80000000u) ready = true;
    }
    if (!ready) { kprintf("sdhci: card never left the busy state\n"); return false; }

    /* BIT 30 OF THE OCR IS THE WHOLE ADDRESSING QUESTION. Set means the card
     * takes block numbers; clear means byte offsets. */
    h->block_addressed = (ocr & 0x40000000u) != 0;

    if (!sd_cmd(h, CMD_ALL_SEND_CID, 0, CMD_RESP_136, false, false)) return false;

    if (h->is_emmc) {
        /* eMMC is TOLD its address; there is only ever one part on the bus. */
        h->rca = 1;
        if (!sd_cmd(h, CMD_SEND_RELATIVE, (uint32_t)h->rca << 16,
                    CMD_RESP_48, false, false)) return false;
    } else {
        if (!sd_cmd(h, CMD_SEND_RELATIVE, 0, CMD_RESP_48, false, false)) return false;
        h->rca = (uint16_t)(r32(h, SD_RESP0) >> 16);
    }

    if (!sd_cmd(h, CMD_SEND_CSD, (uint32_t)h->rca << 16, CMD_RESP_136, false, false))
        return false;

    /* THE RESPONSE REGISTERS HOLD CSD[127:8], SHIFTED DOWN BY EIGHT -- the
     * controller has already dropped the CRC byte. So CSD bit N is bit N-8 of
     * the 120-bit value spread across RESP0..RESP3, and every field offset
     * below is written in those terms rather than in the specification's. */
    uint32_t r0 = r32(h, SD_RESP0), r1 = r32(h, SD_RESP0 + 4);
    uint32_t r2 = r32(h, SD_RESP0 + 8), r3 = r32(h, SD_RESP0 + 12);
    uint32_t csd_structure = (r3 >> 22) & 3u;          /* CSD[127:126] */

    if (csd_structure >= 1) {
        /* Version 2: capacity is (C_SIZE + 1) * 512 KiB, flat. */
        uint32_t c_size = (r1 >> 8) & 0x3FFFFFu;       /* CSD[69:48] */
        h->blocks = ((uint64_t)c_size + 1) * 1024;
    } else {
        /* Version 1: the three-field product, and READ_BL_LEN can exceed 512
         * on old cards -- which is why the block count is computed in bytes
         * and divided, rather than assumed. */
        uint32_t c_size = ((r2 & 0x3u) << 10) | ((r1 >> 22) & 0x3FFu);  /* CSD[73:62] */
        uint32_t c_mult = ((r1 & 0x3u) << 1) | (r0 >> 31);              /* CSD[49:47] */
        uint32_t read_bl_len = (r2 >> 8) & 0xFu;                        /* CSD[83:80] */
        uint64_t bytes = ((uint64_t)c_size + 1) * (1ULL << (c_mult + 2))
                         * (1ULL << read_bl_len);
        h->blocks = bytes / SD_BLOCK_BYTES;
    }

    if (!sd_cmd(h, CMD_SELECT, (uint32_t)h->rca << 16, CMD_RESP_48_BSY, false, true))
        return false;
    /* A block-addressed card has a fixed 512-byte block and rejects CMD16;
     * a byte-addressed one must be told. */
    if (!h->block_addressed &&
        !sd_cmd(h, CMD_SET_BLOCKLEN, SD_BLOCK_BYTES, CMD_RESP_48, false, false))
        return false;
    return true;
}

static bool sdhci_attach(const struct pci_device *pci) {
    if (g_host_count >= SDHCI_MAX_HOSTS) return false;
    struct sdhci_host *h = &g_hosts[g_host_count];
    memset(h, 0, sizeof *h);

    struct pci_bar bar = pci_read_bar(pci->bus, pci->device, pci->function, 0);
    if (!bar.valid || !bar.is_mmio) {
        kprintf("sdhci: %02x:%02x.%u has no MMIO BAR0\n",
                pci->bus, pci->device, pci->function);
        return false;
    }
    uint16_t cmdreg = pci_read16(pci->bus, pci->device, pci->function, PCI_COMMAND);
    pci_write16(pci->bus, pci->device, pci->function, PCI_COMMAND,
                (uint16_t)(cmdreg | 0x0006));   /* memory space + bus master */
    h->regs = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(bar.address, bar.size);
    if (!h->regs) return false;
    h->used = true;

    if (!sd_host_reset(h)) { h->used = false; return false; }

    if (!(r32(h, SD_PRESENT) & PRESENT_CARD_INSERTED)) {
        kprintf("sdhci: host controller v%u.%u at %02x:%02x.%u, no card\n",
                (r16(h, SD_VERSION) & 0xFF) + 1, 0,
                pci->bus, pci->device, pci->function);
        g_host_count++;
        return true;                 /* the controller is up; the slot is empty */
    }

    if (!sd_card_init(h)) {
        kprintf("sdhci: card present but did not initialise\n");
        h->used = false;
        return false;
    }

    h->blk.block_count = h->blocks;
    h->blk.block_size  = SD_BLOCK_BYTES;
    h->blk.read  = sd_blk_read;
    h->blk.write = sd_blk_write;
    h->blk.flush = NULL;
    h->blk.driver_data = h;
    h->blk.dma_max_phys = ~0ULL;      /* PIO: no controller DMA at all */
    h->blk.needs_kernel_range = true;
    if (embk_block_register(&h->blk) != EMBK_OK) { h->used = false; return false; }

    kprintf("sdhci: %s = %s card, %llu blocks x %u B (%llu MiB), %s addressing\n",
            h->blk.name, h->is_emmc ? "eMMC" : "SD",
            (unsigned long long)h->blocks, SD_BLOCK_BYTES,
            (unsigned long long)((h->blocks * SD_BLOCK_BYTES) >> 20),
            h->block_addressed ? "block" : "byte");
    g_host_count++;
    return true;
}

void sdhci_init(void) {
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        /* Class 08 subclass 05 is "SD Host controller". prog-if distinguishes
         * the DMA variants and is not consulted: this driver is PIO either
         * way, and refusing a DMA-capable controller for being DMA-capable
         * would skip the only one QEMU has. */
        if (!d || d->class_code != 0x08 || d->subclass != 0x05) continue;
        sdhci_attach(d);
    }
}

uint32_t sdhci_host_count(void) { return g_host_count; }
bool sdhci_card_present(uint32_t idx) {
    if (idx >= g_host_count) return false;
    return (r32(&g_hosts[idx], SD_PRESENT) & PRESENT_CARD_INSERTED) != 0;
}
uint64_t sdhci_blocks(uint32_t idx) {
    return idx < g_host_count ? g_hosts[idx].blocks : 0;
}
