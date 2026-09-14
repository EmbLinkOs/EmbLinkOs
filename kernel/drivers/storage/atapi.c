/* kernel/drivers/storage/atapi.c -- SCSI commands down an IDE cable.
 *
 * ATAPI is the odd one. An IDE channel speaks ATA: you put an LBA in four
 * registers and issue a one-byte command. An optical drive on that same cable
 * speaks SCSI instead, and ATAPI is the envelope that carries it: issue the
 * ATA command PACKET (0xA0), then write twelve bytes of CDB into the DATA
 * port, and the drive answers as a SCSI target would.
 *
 * So this file is a transport and nothing else. Every command it carries is
 * built by kernel/block/scsi.c, the same code that talks to a USB stick and to
 * virtio-scsi. That is the entire reason the SCSI layer was factored out.
 *
 * TWO THINGS ABOUT OPTICAL MEDIA THAT ARE NOT TRUE OF DISKS:
 *
 *   1. THE BLOCK IS 2048 BYTES, not 512. Every LBA in every command is a
 *      2 KiB block, and a caller that assumes 512 reads four times too little
 *      from four times too far in.
 *   2. THE MEDIUM CAN BE ABSENT. An empty drive is not a broken drive: it
 *      answers INQUIRY perfectly well and fails READ CAPACITY with "no medium
 *      present". That has to read as "no disc", not as a probe failure.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/io.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/atapi.h"
#include "block/block.h"
#include "block/scsi.h"
#include "fs/automount.h"

#define ATA_CMD_PACKET          0xA0
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_STATUS_DF           0x20

#define ATAPI_CDB_LEN     12
#define ATAPI_BLOCK_BYTES 2048u
#define ATAPI_MAX_XFER    (32 * ATAPI_BLOCK_BYTES)

struct atapi_drive {
    bool used;
    uint16_t io_base, ctrl_base;
    bool is_slave;
    struct scsi_device sd;
    struct embk_block_device blk;
    char model[41];
};

static struct atapi_drive g_drives[4];
static uint32_t g_count;
static uint8_t g_xfer[ATAPI_MAX_XFER] __attribute__((aligned(4)));

static void io_delay(uint16_t ctrl) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
}

static bool wait_not_busy(uint16_t io, int spins) {
    for (int i = 0; i < spins; i++) {
        if (!(inb(io + ATA_REG_STATUS) & ATA_STATUS_BSY)) return true;
        __asm__ volatile("pause");
    }
    return false;
}

/* Wait for the drive to want attention: DRQ set (it has data or wants the
 * packet) or BSY clear with DRQ clear (it is finished). Returns the status. */
static uint8_t wait_drq_or_done(uint16_t io, int spins) {
    for (int i = 0; i < spins; i++) {
        uint8_t st = inb(io + ATA_REG_STATUS);
        if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) return st;
        if (!(st & ATA_STATUS_BSY)) return st;
        __asm__ volatile("pause");
    }
    return ATA_STATUS_ERR;
}

static void select_drive(struct atapi_drive *d) {
    outb(d->io_base + ATA_REG_DRIVE, d->is_slave ? 0xB0 : 0xA0);
    io_delay(d->ctrl_base);
}

/* One ATAPI command. `len` is how many bytes the caller expects to move; the
 * drive is free to move fewer and says so per DRQ burst. */
static bool atapi_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                       void *data, uint32_t len, bool to_dev) {
    struct atapi_drive *d = ctx;
    if (len > ATAPI_MAX_XFER) return false;
    uint16_t io = d->io_base;

    select_drive(d);
    if (!wait_not_busy(io, 2000000)) return false;

    outb(io + ATA_REG_ERROR, 0);            /* features: PIO, no overlap */
    outb(io + ATA_REG_SECCOUNT, 0);
    /* THE BYTE-COUNT LIMIT goes in the two LBA registers the ATA path uses
     * for addressing. It caps how much the drive moves per DRQ burst, not how
     * much the command transfers -- a limit of zero means "no limit" on some
     * drives and "nothing" on others, so it is always set. */
    outb(io + ATA_REG_LBA_MID,  (uint8_t)(ATAPI_BLOCK_BYTES & 0xFF));
    outb(io + ATA_REG_LBA_HIGH, (uint8_t)(ATAPI_BLOCK_BYTES >> 8));
    outb(io + ATA_REG_COMMAND, ATA_CMD_PACKET);

    uint8_t st = wait_drq_or_done(io, 2000000);
    if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) return false;
    if (!(st & ATA_STATUS_DRQ)) return false;

    /* The packet is always twelve bytes on the wire even when the command is
     * shorter -- the CDB is zero-padded, not truncated. */
    uint8_t packet[ATAPI_CDB_LEN];
    memset(packet, 0, sizeof packet);
    memcpy(packet, cdb, cdb_len > ATAPI_CDB_LEN ? ATAPI_CDB_LEN : cdb_len);
    for (int i = 0; i < ATAPI_CDB_LEN; i += 2)
        outw(io + ATA_REG_DATA, (uint16_t)(packet[i] | (packet[i+1] << 8)));

    if (to_dev && data && len) memcpy(g_xfer, data, len);

    uint32_t moved = 0;
    for (int burst = 0; burst < 4096; burst++) {
        st = wait_drq_or_done(io, 20000000);
        if (st & (ATA_STATUS_ERR | ATA_STATUS_DF)) return false;
        if (!(st & ATA_STATUS_DRQ)) break;            /* command finished */

        /* HOW MUCH THIS BURST CARRIES is reported by the drive, not chosen by
         * us. Reading a fixed amount is the bug that makes short transfers
         * hang: the drive has 36 bytes of INQUIRY and the host reads 2048. */
        uint32_t n = (uint32_t)inb(io + ATA_REG_LBA_MID) |
                     ((uint32_t)inb(io + ATA_REG_LBA_HIGH) << 8);
        if (!n) break;
        if (moved + n > ATAPI_MAX_XFER) return false;

        for (uint32_t i = 0; i < n; i += 2) {
            if (to_dev) {
                uint16_t w = (uint16_t)(g_xfer[moved + i] |
                                        (g_xfer[moved + i + 1] << 8));
                outw(io + ATA_REG_DATA, w);
            } else {
                uint16_t w = inw(io + ATA_REG_DATA);
                g_xfer[moved + i]     = (uint8_t)w;
                g_xfer[moved + i + 1] = (uint8_t)(w >> 8);
            }
        }
        moved += n;
    }

    if (inb(io + ATA_REG_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF)) return false;
    if (!to_dev && data && len) {
        uint32_t n = moved < len ? moved : len;
        memcpy(data, g_xfer, n);
        /* A short answer is not an error -- INQUIRY legitimately returns less
         * than was asked for -- but the tail must be zero rather than the
         * previous command's bytes. */
        if (n < len) memset((uint8_t *)data + n, 0, len - n);
    }
    return true;
}

static int atapi_blk_read(struct embk_block_device *b, uint64_t lba,
                          uint32_t count, void *buf) {
    struct atapi_drive *d = b->driver_data;
    return scsi_read(&d->sd, lba, count, buf, ATAPI_MAX_XFER / ATAPI_BLOCK_BYTES);
}
static int atapi_blk_write(struct embk_block_device *b, uint64_t lba,
                           uint32_t count, const void *buf) {
    (void)b; (void)lba; (void)count; (void)buf;
    return -EMBK_EROFS;      /* a burner is a different driver entirely */
}

/* Is there an ATAPI device at this position? The ATA IDENTIFY command aborts
 * on a packet device and leaves a SIGNATURE in the cylinder registers --
 * 0x14/0xEB -- which is the only reliable way to tell one from an absent
 * drive, because both fail IDENTIFY. */
static bool atapi_present(uint16_t io, uint16_t ctrl, bool slave) {
    outb(io + ATA_REG_DRIVE, slave ? 0xB0 : 0xA0);
    io_delay(ctrl);
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA_LOW, 0);
    outb(io + ATA_REG_LBA_MID, 0);
    outb(io + ATA_REG_LBA_HIGH, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    if (inb(io + ATA_REG_STATUS) == 0) return false;     /* nothing there */
    if (!wait_not_busy(io, 1000000)) return false;

    bool is_atapi = inb(io + ATA_REG_LBA_MID) == 0x14 &&
                    inb(io + ATA_REG_LBA_HIGH) == 0xEB;

    /* LEAVE THE CHANNEL EXACTLY AS IT WAS FOUND.
     *
     * A HARD DISK ANSWERS THIS IDENTIFY PERFECTLY WELL. It then sits with 512
     * bytes in its buffer and DRQ asserted, waiting for somebody to read
     * them. A drive in that state ignores the next command entirely -- no
     * error, no interrupt -- and the ATA driver, which waits on an interrupt
     * that will never arrive, hangs the machine on the first disk read after
     * boot. That is what this probe did before the drain below existed, and
     * it did it to the boot disk, so nothing at all worked.
     *
     * 256 reads is the entire cost of not doing that. */
    if (inb(io + ATA_REG_STATUS) & ATA_STATUS_DRQ)
        for (int i = 0; i < 256; i++) (void)inw(io + ATA_REG_DATA);
    (void)inb(io + ATA_REG_ERROR);    /* clears a latched abort */
    (void)inb(io + ATA_REG_STATUS);   /* and the pending interrupt with it */
    return is_atapi;
}

static void atapi_attach(uint16_t io, uint16_t ctrl, bool slave) {
    if (g_count >= 4) return;
    struct atapi_drive *d = &g_drives[g_count];
    memset(d, 0, sizeof *d);
    d->io_base = io; d->ctrl_base = ctrl; d->is_slave = slave;
    d->sd.exec = atapi_exec;
    d->sd.ctx = d;
    d->used = true;

    if (!scsi_probe(&d->sd)) {
        /* The drive is there and INQUIRY may well have worked -- what failed
         * is READ CAPACITY, which is what an EMPTY drive does. Say so. */
        kprintf("atapi: drive at %03x%s reported no medium\n",
                io, slave ? " (slave)" : "");
        g_count++;                 /* still counted: the drive exists */
        return;
    }

    if (d->sd.block_size != ATAPI_BLOCK_BYTES)
        kprintf("atapi: unusual block size %u (expected 2048)\n", d->sd.block_size);

    d->blk.block_count = d->sd.blocks;
    d->blk.block_size  = d->sd.block_size;
    d->blk.read  = atapi_blk_read;
    d->blk.write = atapi_blk_write;
    d->blk.flush = NULL;
    d->blk.driver_data = d;
    d->blk.dma_max_phys = ~0ULL;        /* PIO */
    d->blk.needs_kernel_range = true;
    if (embk_block_register(&d->blk) != EMBK_OK) { d->used = false; return; }

    kprintf("atapi: %s = %s %s, %llu x %u B disc\n", d->blk.name,
            d->sd.vendor, d->sd.product,
            (unsigned long long)d->sd.blocks, d->sd.block_size);
    /* A disc with a filesystem on it is worth reading. automount now knows
     * ISO 9660 and tries it first when the medium's block is 2048. */
    automount_attach(&d->blk);
    g_count++;
}

void atapi_init(void) {
    static const uint16_t io[2]   = { ATA_PRIMARY_IO, ATA_SECONDARY_IO };
    static const uint16_t ctrl[2] = { ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL };
    for (int ch = 0; ch < 2; ch++)
        for (int sl = 0; sl < 2; sl++)
            if (atapi_present(io[ch], ctrl[ch], sl != 0))
                atapi_attach(io[ch], ctrl[ch], sl != 0);
}

uint32_t atapi_drive_count(void) { return g_count; }
const char *atapi_drive_name(uint32_t i) {
    return (i < g_count && g_drives[i].blk.name[0]) ? g_drives[i].blk.name : NULL;
}
uint64_t atapi_drive_blocks(uint32_t i) {
    return i < g_count ? g_drives[i].sd.blocks : 0;
}
