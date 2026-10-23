/* kernel/drivers/storage/virtio_scsi.c -- the other standard VM disk.
 *
 * virtio-blk is one disk behind one queue and nothing else: no command set, no
 * targets, no removable media. virtio-scsi is a CONTROLLER -- it carries real
 * SCSI commands to as many targets as the host attached, which is why it is
 * what a VM uses when it wants more than one disk, a CD-ROM, or anything that
 * can be added while the machine runs.
 *
 * The command set is kernel/block/scsi.c, shared with USB mass storage. This
 * file is only the envelope: a request header, the CDB, the data, and a
 * response header coming back.
 *
 * THE QUEUE LAYOUT IS THE SPECIFICATION'S AND NOT NEGOTIABLE. Queue 0 is
 * control, queue 1 is the event queue, and request queues start at 2. Putting
 * a command on queue 0 is not a slower path; it is a different conversation
 * that the device answers with nothing.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/virtio_pci.h"
#include "block/block.h"
#include "block/scsi.h"
#include "fs/automount.h"
#include "mm/pmm.h"

#define VIRTIO_SCSI_DEVID_M 0x1048   /* 0x1040 + device type 8 */
#define VIRTIO_SCSI_DEVID_T 0x1004   /* transitional / legacy  */

#define VS_QUEUE_REQUEST 2
#define VS_QSIZE 16
#define VS_MAX_TARGETS 8
#define VS_XFER_MAX 65536            /* one command's data, capped */

/* The request header the device expects, then the CDB. cdb_size is fixed by
 * the device's configuration space and is 32 on every implementation, so the
 * CDB is padded rather than sized per command. */
struct vs_req_hdr {
    uint8_t  lun[8];
    uint64_t id;
    uint8_t  task_attr;
    uint8_t  prio;
    uint8_t  crn;
    uint8_t  cdb[32];
} __attribute__((packed));

struct vs_resp_hdr {
    uint32_t sense_len;
    uint32_t residual;
    uint16_t status_qualifier;
    uint8_t  status;             /* the SCSI status: 0 is GOOD */
    uint8_t  response;           /* the virtio-level result: 0 is OK */
    uint8_t  sense[96];
} __attribute__((packed));

static struct virtio_pci_dev g_vd;
static bool g_up;

static struct vring_desc g_desc[VS_QSIZE] __attribute__((aligned(16)));
static struct {
    uint16_t flags, idx;
    uint16_t ring[VS_QSIZE];
    uint16_t used_event;
} __attribute__((packed, aligned(2))) g_avail;
static struct {
    uint16_t flags, idx;
    struct vring_used_elem ring[VS_QSIZE];
    uint16_t avail_event;
} __attribute__((packed, aligned(4))) g_used;

static struct vs_req_hdr  g_req  __attribute__((aligned(64)));
static struct vs_resp_hdr g_resp __attribute__((aligned(64)));
static uint8_t  g_data[VS_XFER_MAX] __attribute__((aligned(64)));
static uint16_t g_notify_off, g_qsize, g_last_used;

struct vs_target {
    bool used;
    uint8_t target;
    struct scsi_device sd;
    struct embk_block_device blk;
};
static struct vs_target g_targets[VS_MAX_TARGETS];

static inline uint64_t dma(const volatile void *p) {
    return KV2P((uint64_t)(uintptr_t)p);
}

/* One command. The descriptor chain is: [out: header][out: data if writing]
 * [in: response][in: data if reading] -- the device reads the out-descriptors
 * and writes the in-ones, and the ORDER within each group is what the
 * specification fixes. */
static bool vs_exec(void *ctx, const uint8_t *cdb, uint8_t cdb_len,
                    void *data, uint32_t len, bool to_dev) {
    struct vs_target *t = ctx;
    if (!g_up || len > VS_XFER_MAX) return false;

    memset(&g_req, 0, sizeof g_req);
    /* LUN addressing: byte 0 is always 1, byte 1 the target, bytes 2-3 the
     * LUN in a format with its own two top bits set. Only LUN 0 is used. */
    g_req.lun[0] = 1;
    g_req.lun[1] = t->target;
    g_req.lun[2] = 0x40;
    g_req.lun[3] = 0;
    g_req.id = 1;
    memcpy(g_req.cdb, cdb, cdb_len > 32 ? 32 : cdb_len);

    if (to_dev && data && len) memcpy(g_data, data, len);
    memset(&g_resp, 0, sizeof g_resp);

    uint16_t n = 0;
    g_desc[0].addr = dma(&g_req); g_desc[0].len = sizeof g_req;
    g_desc[0].flags = VRING_DESC_F_NEXT; g_desc[0].next = 1; n = 1;

    if (to_dev && len) {
        g_desc[n].addr = dma(g_data); g_desc[n].len = len;
        g_desc[n].flags = VRING_DESC_F_NEXT; g_desc[n].next = (uint16_t)(n + 1);
        n++;
    }
    g_desc[n].addr = dma(&g_resp); g_desc[n].len = sizeof g_resp;
    g_desc[n].flags = VRING_DESC_F_WRITE;
    if (!to_dev && len) {
        g_desc[n].flags |= VRING_DESC_F_NEXT; g_desc[n].next = (uint16_t)(n + 1);
        n++;
        g_desc[n].addr = dma(g_data); g_desc[n].len = len;
        g_desc[n].flags = VRING_DESC_F_WRITE;
    }
    g_desc[n].next = 0;

    g_avail.ring[g_avail.idx % g_qsize] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify_off, VS_QUEUE_REQUEST);

    for (int spin = 0; spin < 20000000; spin++) {
        if (g_used.idx != g_last_used) { g_last_used++; goto done; }
        __asm__ volatile("" ::: "memory");
    }
    kprintf("virtio-scsi: target %u did not answer command 0x%02x\n",
            t->target, cdb[0]);
    return false;

done:
    /* TWO STATUS BYTES AND BOTH MATTER. `response` is the controller's --
     * "I could not deliver this" -- and `status` is the target's answer to the
     * command itself. A driver that checks only one reports a failed read as
     * success about half the time. */
    if (g_resp.response != 0 || g_resp.status != 0) return false;
    if (!to_dev && data && len) memcpy(data, g_data, len);
    return true;
}

static int vs_blk_read(struct embk_block_device *dev, uint64_t lba,
                       uint32_t count, void *buffer) {
    struct vs_target *t = dev->driver_data;
    return scsi_read(&t->sd, lba, count, buffer, VS_XFER_MAX / t->sd.block_size);
}
static int vs_blk_write(struct embk_block_device *dev, uint64_t lba,
                        uint32_t count, const void *buffer) {
    struct vs_target *t = dev->driver_data;
    return scsi_write(&t->sd, lba, count, buffer, VS_XFER_MAX / t->sd.block_size);
}

bool virtio_scsi_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_SCSI_DEVID_M, 0);
    if (!pci) pci = virtio_pci_find(VIRTIO_SCSI_DEVID_T, 0);
    if (!pci) return false;

    if (!virtio_pci_attach(&g_vd, pci, "virtio-scsi", 0, 0)) return false;

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)&g_avail, 0, sizeof g_avail);
    memset((void *)&g_used, 0, sizeof g_used);
    g_avail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    /* THE FIRST REQUEST QUEUE IS NUMBER 2. Queues 0 and 1 are control and
     * events; a command placed on either is a different conversation and the
     * device answers it with nothing at all. */
    g_qsize = virtio_pci_setup_queue(&g_vd, VS_QUEUE_REQUEST, VS_QSIZE,
                                     g_desc, &g_avail, &g_used, &g_notify_off);
    if (!g_qsize) { kprintf("virtio-scsi: no request queue\n"); return false; }
    virtio_pci_driver_ok(&g_vd);
    g_last_used = g_used.idx;
    g_up = true;

    /* WALK THE TARGETS. The controller reports how many it can address; each
     * one that answers INQUIRY becomes a block device. A target that is not
     * there simply fails, which is not an error worth printing per slot. */
    uint32_t max_target = 4;
    if (g_vd.devcfg) {
        uint32_t mt = vp_r32(g_vd.devcfg, 12);   /* max_target */
        if (mt && mt < VS_MAX_TARGETS) max_target = mt + 1;
    }
    if (max_target > VS_MAX_TARGETS) max_target = VS_MAX_TARGETS;

    int found = 0;
    for (uint32_t tn = 0; tn < max_target; tn++) {
        struct vs_target *t = &g_targets[tn];
        t->used = true;
        t->target = (uint8_t)tn;
        t->sd.exec = vs_exec;
        t->sd.ctx = t;
        if (!scsi_probe(&t->sd)) { t->used = false; continue; }

        /* An optical target used to be reported and dropped, because there
         * was no filesystem that could read one. kernel/fs/iso9660.c is that
         * filesystem, so it is now registered like any other block device and
         * automount decides what is on it. Its block is 2048, which the block
         * layer has always carried per device rather than assumed. */
        if (t->sd.is_optical)
            kprintf("virtio-scsi: target %u is an optical drive (%s %s)\n",
                    tn, t->sd.vendor, t->sd.product);

        memset(&t->blk, 0, sizeof t->blk);
        t->blk.block_count = t->sd.blocks;
        t->blk.block_size  = t->sd.block_size;
        t->blk.read  = vs_blk_read;
        t->blk.write = vs_blk_write;
        t->blk.flush = NULL;
        t->blk.driver_data = t;
        t->blk.dma_max_phys = ~0ULL;
        t->blk.needs_kernel_range = true;
        if (embk_block_register(&t->blk) != EMBK_OK) { t->used = false; continue; }

        kprintf("virtio-scsi: %s = target %u, %s %s, %llu x %u B\n",
                t->blk.name, tn, t->sd.vendor, t->sd.product,
                (unsigned long long)t->sd.blocks, t->sd.block_size);
        if (t->sd.is_optical) automount_attach(&t->blk);
        found++;
    }

    if (!found) kprintf("virtio-scsi: controller up, no targets answered\n");
    return true;
}

bool virtio_scsi_present(void) { return g_up; }

uint32_t virtio_scsi_target_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < VS_MAX_TARGETS; i++) if (g_targets[i].used) n++;
    return n;
}
