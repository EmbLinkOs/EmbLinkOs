/* kernel/drivers/usb/usb_uas.c -- USB Attached SCSI.
 *
 * Bulk-Only Transport, which this kernel already speaks, carries ONE command
 * at a time and wraps it in a 31-byte envelope with a 13-byte reply. It was
 * designed for floppy-speed flash and it is what every USB 2 stick does.
 *
 * UAS is what a USB 3 stick negotiates instead. The difference that matters is
 * not the envelope -- it is that commands are TAGGED and travel on their own
 * pipe, so a device can have several in flight and answer them out of order.
 * BOT cannot do that at all: the next command cannot start until the previous
 * CSW has been read.
 *
 * This driver issues one command at a time anyway, because everything above it
 * is synchronous. What it buys today is the correct protocol on a device that
 * asked for it, and a tag field that a queued implementation can start using
 * without touching the wire format.
 *
 * FOUR BULK PIPES, AND THEY ARE NOT INTERCHANGEABLE: command (host writes),
 * status (host reads), data-in, data-out. Nothing in the endpoint descriptors
 * distinguishes them -- the class-specific "pipe usage" descriptor that
 * follows each one does, and usb_core.c now records it as ep->pipe_id.
 *
 * ON USB 2 THERE ARE NO BULK STREAMS, so the device announces when it is ready
 * to move data by sending a READ READY or WRITE READY information unit on the
 * status pipe. Skipping that and transferring immediately is the mistake that
 * makes a UAS driver appear to work and then hang under load.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/usb/usb_core.h"
#include "block/block.h"
#include "block/scsi.h"
#include "fs/automount.h"

#define UAS_MAX_DEVS   2
#define UAS_CHUNK      4096U

/* Information unit ids (UAS 1.0, table 4). */
#define IU_COMMAND      0x01
#define IU_SENSE        0x03
#define IU_RESPONSE     0x04
#define IU_TASK_MGMT    0x05
#define IU_READ_READY   0x06
#define IU_WRITE_READY  0x07

struct uas {
    bool used;
    struct usb_device *dev;
    uint8_t ep_cmd, ep_status, ep_in, ep_out;
    uint16_t tag;
    struct scsi_device sd;
    struct embk_block_device blk;
    uint8_t iu[64]   __attribute__((aligned(64)));
    uint8_t data[UAS_CHUNK] __attribute__((aligned(64)));
};

static struct uas g_uas[UAS_MAX_DEVS];

static inline void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* One command. Command IU out, then whatever the status pipe says to do, then
 * the sense IU that ends it. */
static bool uas_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                     void *data, uint32_t len, bool to_dev) {
    struct uas *u = ctx;
    if (!u->used || len > UAS_CHUNK) return false;
    struct usb_device *dev = u->dev;

    uint16_t tag = ++u->tag;
    if (!tag) tag = u->tag = 1;        /* TAG 0 IS RESERVED and never matches */

    /* Command IU. 16 bytes of header then the CDB; a CDB longer than 16 needs
     * the additional-length field, which nothing here issues. */
    memset(u->iu, 0, 32);
    u->iu[0] = IU_COMMAND;
    put_be16(&u->iu[2], tag);
    u->iu[3 + 0] = 0;                  /* task attribute: simple */
    u->iu[6] = 0;                      /* additional CDB length / 4 */
    /* LUN in the SCSI "single level, peripheral device" format: 8 bytes, all
     * zero for LUN 0. Written out because a zeroed buffer that happens to be
     * right is not the same as a value that was chosen. */
    u->iu[8] = 0;
    memcpy(&u->iu[16], cdb, cdb_len > 16 ? 16 : cdb_len);
    if (dev->ops->bulk(dev, u->ep_cmd, u->iu, 32) < 0) return false;

    if (to_dev && data && len) memcpy(u->data, data, len);

    /* Read status IUs until one ends the command. At most three: a ready IU,
     * then the sense IU, plus one spare for a response IU. */
    for (int round = 0; round < 3; round++) {
        int got = dev->ops->bulk(dev, u->ep_status, u->iu, sizeof u->iu);
        if (got < 4) return false;
        uint16_t rtag = (uint16_t)((u->iu[2] << 8) | u->iu[3]);
        if (rtag != tag) continue;     /* somebody else's answer; not ours */

        switch (u->iu[0]) {
        case IU_READ_READY:
            if (!len) break;
            if (dev->ops->bulk(dev, u->ep_in, u->data, len) < 0) return false;
            break;
        case IU_WRITE_READY:
            if (!len) break;
            if (dev->ops->bulk(dev, u->ep_out, u->data, len) < 0) return false;
            break;
        case IU_SENSE: {
            /* Byte 6 is the SCSI status. GOOD is 0; anything else means the
             * target refused, and the sense data after it says why -- which
             * this kernel does not act on beyond "it failed". */
            uint8_t status = u->iu[6];
            if (status != 0) return false;
            if (!to_dev && data && len) memcpy(data, u->data, len);
            return true;
        }
        case IU_RESPONSE:
            /* The device declined the command itself (bad IU, bad tag, task
             * management). Not a target error -- a protocol one. */
            return false;
        default:
            return false;
        }
    }
    return false;
}

static int uas_read(struct embk_block_device *b, uint64_t lba,
                    uint32_t count, void *buf) {
    struct uas *u = b->driver_data;
    return scsi_read(&u->sd, lba, count, buf, UAS_CHUNK / u->sd.block_size);
}
static int uas_write(struct embk_block_device *b, uint64_t lba,
                     uint32_t count, const void *buf) {
    struct uas *u = b->driver_data;
    return scsi_write(&u->sd, lba, count, buf, UAS_CHUNK / u->sd.block_size);
}

bool usb_uas_attach(struct usb_device *dev) {
    /* FIND THE PIPES BY WHAT THE DEVICE CALLED THEM. Falling back to "first
     * bulk IN, first bulk OUT" would attach to whichever two happened to come
     * first and then hang, which is worse than not attaching. */
    uint8_t cmd = 0, status = 0, in = 0, out = 0;
    for (uint8_t i = 0; i < dev->num_eps; i++) {
        const struct usb_ep_info *ep = &dev->eps[i];
        if ((ep->attr & 3) != 2) continue;          /* bulk only */
        switch (ep->pipe_id) {
        case 1: cmd = ep->addr; break;
        case 2: status = ep->addr; break;
        case 3: in = ep->addr; break;
        case 4: out = ep->addr; break;
        default: break;
        }
    }
    if (!cmd || !status || !in || !out) {
        kprintf("UAS: device declared protocol 0x62 but named %s%s%s%s pipes"
                " -- falling back to Bulk-Only\n",
                cmd ? "" : "no command ", status ? "" : "no status ",
                in ? "" : "no data-in ", out ? "" : "no data-out ");
        return false;
    }

    struct uas *u = NULL;
    for (int i = 0; i < UAS_MAX_DEVS; i++)
        if (!g_uas[i].used) { u = &g_uas[i]; break; }
    if (!u) { kprintf("UAS: device table full\n"); return false; }

    memset(u, 0, sizeof *u);
    u->used = true;
    u->dev = dev;
    u->ep_cmd = cmd; u->ep_status = status; u->ep_in = in; u->ep_out = out;
    u->sd.exec = uas_exec;
    u->sd.ctx = u;

    if (!scsi_probe(&u->sd)) {
        kprintf("UAS: target did not answer INQUIRY\n");
        u->used = false;
        return false;
    }
    if (u->sd.block_size == 0 || u->sd.block_size > UAS_CHUNK) {
        kprintf("UAS: unsupported block size %u\n", u->sd.block_size);
        u->used = false;
        return false;
    }

    memset(&u->blk, 0, sizeof u->blk);
    u->blk.block_count = u->sd.blocks;
    u->blk.block_size  = u->sd.block_size;
    u->blk.read  = uas_read;
    u->blk.write = uas_write;
    u->blk.driver_data = u;
    u->blk.dma_max_phys = ~0ULL;
    u->blk.needs_kernel_range = true;
    if (embk_block_register(&u->blk) != EMBK_OK) {
        kprintf("UAS: block registration failed\n");
        u->used = false;
        return false;
    }

    kprintf("UAS: %s = %s %s, %llu x %u B\n", u->blk.name,
            u->sd.vendor, u->sd.product,
            (unsigned long long)u->sd.blocks, u->sd.block_size);
    /* And make it reachable, the same way a BOT stick is: the point of
     * plugging something in is to read what is on it. */
    automount_attach(&u->blk);
    return true;
}

void usb_uas_detach(struct usb_device *dev) {
    for (int i = 0; i < UAS_MAX_DEVS; i++) {
        struct uas *u = &g_uas[i];
        if (!u->used || u->dev != dev) continue;
        automount_detach(&u->blk);
        embk_block_unregister(&u->blk);
        kprintf("UAS: %s removed\n", u->blk.name);
        u->used = false;
        u->dev = NULL;
    }
}

uint32_t usb_uas_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < UAS_MAX_DEVS; i++) if (g_uas[i].used) n++;
    return n;
}
