#include "drivers/storage/virtio_blk.h"
#include "drivers/bus/pci.h"
#include "drivers/bus/virtio_pci.h"
#include "block/block.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/arch_irq.h"
#include "process/ksync.h"    /* the single request slot needs a SLEEPING lock */

/* virtio-blk over modern virtio-PCI.
 *
 * Written for aarch64 -- QEMU `virt` has no ATA and no AHCI, so without this
 * the port has no disk and therefore no filesystem and therefore nothing to
 * run. It is SHARED code, not arch code: virtio is a bus protocol, and x86 can
 * use the same driver by attaching a virtio-blk-pci device.
 *
 * POLLED, deliberately. A block read already costs a device round trip, and
 * the interrupt path would buy latency this kernel cannot yet spend: the only
 * caller is the filesystem, which is synchronous, and an interrupt-driven
 * version would need a wait queue and a completion per request for no
 * throughput gain at one outstanding request. The x86 ATA driver blocks on an
 * IRQ for the same amount of work; this spins instead, which on a virtio
 * device is microseconds.
 *
 * The virtio-PCI capability walk below is the THIRD copy in this tree
 * (virtio_net.c and virtio_gpu.c have their own). That duplication is real and
 * recorded in docs/TODO.md; it is not fixed here because factoring it means
 * editing two working drivers on the architecture that is not being ported. */

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_BLK_DEVID_T  0x1001      /* transitional */
#define VIRTIO_BLK_DEVID_M  0x1042      /* modern (1.0+) */

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VC_DEVICE_FEATURE_SELECT 0x00
#define VC_DEVICE_FEATURE        0x04
#define VC_DRIVER_FEATURE_SELECT 0x08
#define VC_DRIVER_FEATURE        0x0C
#define VC_MSIX_CONFIG           0x10
#define VC_NUM_QUEUES            0x12
#define VC_DEVICE_STATUS         0x14
#define VC_QUEUE_SELECT          0x16
#define VC_QUEUE_SIZE            0x18
#define VC_QUEUE_MSIX_VECTOR     0x1A
#define VC_QUEUE_ENABLE          0x1C
#define VC_QUEUE_NOTIFY_OFF      0x1E
#define VC_QUEUE_DESC            0x20
#define VC_QUEUE_DRIVER          0x28
#define VC_QUEUE_DEVICE          0x30

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1_BIT  (1U << 0)   /* feature bit 32 -> bank 1, bit 0 */
#define VIRTIO_MSI_NO_VECTOR    0xFFFF

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VQ_SIZE   64
#define SECTOR    512

/* virtio-blk request header, then data, then a one-byte status. Three
 * descriptors, because the device must be able to WRITE the status and (for a
 * read) the data, while the header is always read-only to it -- and a
 * descriptor carries one direction. */
struct vblk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

#define VIRTIO_BLK_T_IN    0
#define VIRTIO_BLK_T_OUT   1
#define VIRTIO_BLK_T_FLUSH 4

/* struct vring_desc and struct vring_used_elem come from virtio_pci.h -- they
 * are the SPEC's layouts and belong with the transport. The two below are this
 * driver's, because their ring[] is sized by VQ_SIZE and a queue depth is a
 * driver's choice, not the transport's. */
struct vring_avail { uint16_t flags; uint16_t idx; uint16_t ring[VQ_SIZE]; } __attribute__((packed));
struct vring_used  { uint16_t flags; uint16_t idx; struct vring_used_elem ring[VQ_SIZE]; } __attribute__((packed));

/* ONE OF THESE PER DEVICE. The first version kept everything in file-scope
 * singletons -- one ring, one bounce buffer, one header, one status byte,
 * one lock -- and could therefore drive exactly one disk: the second
 * virtio-blk on the machine was found by the PCI scan and never looked at
 * again. A second disk is how aarch64 gets a swap store and how the
 * crash-consistency test gets its seed image, so the singletons became an
 * array of instances, one attached per matching PCI function.
 *
 * Rings live in .bss so KV2P() can turn them into the physical addresses the
 * device needs; a kmalloc'd ring would be in the heap window, which is mapped
 * but not KV2P-able. Alignment is the virtio spec's. One bounce buffer per
 * device: callers hand us kernel pointers that may not be physically
 * contiguous past a page, and a descriptor addresses physical memory. 64 KiB
 * matches the filesystem's largest read. */
#define BOUNCE_SZ (64u * 1024u)
#define VBLK_MAX  4

struct vblk {
    struct vring_desc  desc[VQ_SIZE]  __attribute__((aligned(16)));
    struct vring_avail avail          __attribute__((aligned(2)));
    struct vring_used  used           __attribute__((aligned(4)));
    uint8_t            bounce[BOUNCE_SZ] __attribute__((aligned(4096)));
    struct vblk_req_hdr hdr           __attribute__((aligned(16)));
    volatile uint8_t   status         __attribute__((aligned(16)));

    struct virtio_pci_dev vp;   /* the shared transport: windows + handshake */
    volatile uint8_t *common, *notify, *devcfg;
    uint32_t notify_multiplier;
    uint16_t notify_off;
    uint16_t qsize;
    uint16_t last_used;
    uint64_t capacity;          /* in 512-byte sectors */
    bool     up;

    struct embk_block_device blkdev;

    /* ONE REQUEST AT A TIME PER DEVICE, and it has to be enforced rather than
     * assumed. Everything this driver submits with lives in the instance --
     * one descriptor table, one avail ring, one header, one status byte, one
     * last_used cursor. That was true and safe while the only caller was the
     * boot thread. It stopped being either the moment there was a real
     * userland: init, the desktop, TopBar and posixdemo all touch the
     * filesystem, the syscall path runs with interrupts ENABLED, and the
     * request SPINS waiting for the device -- which is exactly where a timer
     * tick preempts it. A second thread then overwrites the header and
     * avail.idx underneath the first, and both wait for a completion that
     * will never match. It shows up as "virtio-blk: request timed out", far
     * from the cause.
     *
     * A SLEEPING lock, not a spinlock: the wait is a disk round trip, and
     * holding a spinlock (with interrupts off) across one would stop the
     * scheduler for its duration. ksync's mutex handles the pre-scheduler
     * case -- current_thread is NULL then and the lock is uncontended, so it
     * neither blocks nor complains -- which matters because this driver is
     * probed before process_init(). */
    struct mutex lock;
};

static struct vblk g_vblks[VBLK_MAX];
static int         g_nvblk;

static inline uint8_t  vr8 (volatile uint8_t *b, uint32_t o) { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t vr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline uint64_t vr64(volatile uint8_t *b, uint32_t o) {
    return (uint64_t)vr32(b, o) | ((uint64_t)vr32(b, o + 4) << 32);
}
static inline void vw8 (volatile uint8_t *b, uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(b + o) = v; }
static inline void vw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void vw64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    vw32(b, o, (uint32_t)v); vw32(b, o + 4, (uint32_t)(v >> 32));
}
static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static int vblk_request(struct vblk *v, uint32_t type, uint64_t sector, void *data,
                        uint32_t len, bool device_writes) {
    if (!v->up)
        return -EMBK_EIO;

    mutex_lock(&v->lock);

    v->hdr.type     = type;
    v->hdr.reserved = 0;
    v->hdr.sector   = sector;
    v->status       = 0xFF;          /* so "unchanged" is distinguishable */

    /* TWO descriptors for a request with no payload, THREE otherwise.
     *
     * FLUSH carries no data, and the natural-looking thing -- keep the
     * three-descriptor shape and set the middle one's length to zero -- is
     * INVALID: the virtio spec has no zero-length descriptor, and QEMU rejects
     * the whole request rather than completing it. The driver then spins out
     * its (very generous) timeout, and the device stays in an error state, so
     * EVERY REQUEST AFTER IT FAILS TOO. That is why the symptom was a wall of
     * "request timed out (type 0 sector N)" reads with one flush buried at the
     * top of it.
     *
     * Latent until userland could write: the desktop only reads, and the flush
     * comes from EMBKFS's pre-commit. posixdemo's first mkdir found it. */
    uint16_t status_desc = len ? 2 : 1;

    v->desc[0].addr  = dma(&v->hdr);
    v->desc[0].len   = sizeof(v->hdr);
    v->desc[0].flags = VRING_DESC_F_NEXT;
    /* Always 1: with a payload that is the data descriptor and the status
     * follows it at 2; without one, index 1 IS the status descriptor. */
    v->desc[0].next  = 1;

    if (len) {
        v->desc[1].addr  = dma(data);
        v->desc[1].len   = len;
        v->desc[1].flags = VRING_DESC_F_NEXT | (device_writes ? VRING_DESC_F_WRITE : 0);
        v->desc[1].next  = 2;
    }

    v->desc[status_desc].addr  = dma(&v->status);
    v->desc[status_desc].len   = 1;
    v->desc[status_desc].flags = VRING_DESC_F_WRITE;
    v->desc[2].next  = 0;

    uint16_t slot = v->avail.idx % v->qsize;
    v->avail.ring[slot] = 0;
    __sync_synchronize();
    v->avail.idx++;
    __sync_synchronize();

    vw16(v->notify, (uint32_t)v->notify_off * v->notify_multiplier, 0);

    /* Spin for the completion. The bound is generous and exists only so a
     * device that never answers produces an error instead of a hung kernel --
     * a wedged disk must not be indistinguishable from a wedged machine. */
    for (uint64_t spins = 0; spins < 200000000ULL; spins++) {
        __sync_synchronize();
        if (v->used.idx != v->last_used) {
            v->last_used = v->used.idx;
            __sync_synchronize();
            int rc = (v->status == 0) ? EMBK_OK : -EMBK_EIO;
            mutex_unlock(&v->lock);
            return rc;
        }
        arch_cpu_relax();
    }
    kprintf("virtio-blk: %s: request timed out (type %d sector %d)\n",
            v->blkdev.name, (int)type, (int)sector);
    mutex_unlock(&v->lock);
    return -EMBK_EIO;
}

static int vblk_read(struct embk_block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    struct vblk *v = (struct vblk *)dev->driver_data;
    uint8_t *out = (uint8_t *)buf;
    while (count) {
        uint32_t chunk = count > (BOUNCE_SZ / SECTOR) ? (BOUNCE_SZ / SECTOR) : count;
        int rc = vblk_request(v, VIRTIO_BLK_T_IN, lba, v->bounce, chunk * SECTOR, true);
        if (rc != EMBK_OK)
            return rc;
        memcpy(out, v->bounce, chunk * SECTOR);
        out   += chunk * SECTOR;
        lba   += chunk;
        count -= chunk;
    }
    return EMBK_OK;
}

static int vblk_write(struct embk_block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct vblk *v = (struct vblk *)dev->driver_data;
    const uint8_t *in = (const uint8_t *)buf;
    while (count) {
        uint32_t chunk = count > (BOUNCE_SZ / SECTOR) ? (BOUNCE_SZ / SECTOR) : count;
        memcpy(v->bounce, in, chunk * SECTOR);
        int rc = vblk_request(v, VIRTIO_BLK_T_OUT, lba, v->bounce, chunk * SECTOR, false);
        if (rc != EMBK_OK)
            return rc;
        in    += chunk * SECTOR;
        lba   += chunk;
        count -= chunk;
    }
    return EMBK_OK;
}

static int vblk_flush(struct embk_block_device *dev) {
    struct vblk *v = (struct vblk *)dev->driver_data;
    /* No buffer and no length: vblk_request() builds the two-descriptor chain
     * a payload-free request requires. The bounce buffer used to be passed
     * here as a dummy, which read as harmless and was not -- see the
     * descriptor note. */
    return vblk_request(v, VIRTIO_BLK_T_FLUSH, 0, 0, 0, false);
}

/* Attach one PCI function as instance `v`. */
static bool vblk_attach(struct vblk *v, const struct pci_device *dev) {
    /* --- the transport ------------------------------------------------------
     * The capability walk, the three config windows, bus mastering and the
     * status handshake all live in drivers/bus/virtio_pci.c now. This driver
     * used to carry its own copy, and so did virtio-net and virtio-gpu; the
     * fourth copy is what docs/TODO.md said would be one too many.
     *
     * Zero requested features, deliberately: a virtio-blk with no negotiated
     * options is the one whose request format cannot be subtly wrong, and
     * nothing here needs discard, write-zeroes or multi-queue to read a
     * sector. */
    mutex_init(&v->lock);
    if (!virtio_pci_attach(&v->vp, dev, "virtio-blk", 0, 0))
        return false;
    v->common = v->vp.common;
    v->notify = v->vp.notify;
    v->devcfg = v->vp.devcfg;
    v->notify_multiplier = v->vp.notify_multiplier;

    /* This driver DOES need the device config: capacity is its first field. */
    if (!v->devcfg) {
        kprintf("virtio-blk: device exposes no device-config window\n");
        return false;
    }

    /* --- queue 0 ------------------------------------------------------------ */
    memset(v->desc, 0, sizeof(v->desc));
    memset((void *)&v->avail, 0, sizeof(v->avail));
    memset((void *)&v->used, 0, sizeof(v->used));
    v->last_used = 0;

    /* VRING_AVAIL_F_NO_INTERRUPT. This driver POLLS, so it must tell the
     * device not to signal completions -- otherwise the device raises its
     * legacy INTx line on every request and, with no handler to clear a
     * LEVEL-triggered interrupt, the machine stops making progress. A polled
     * driver that forgets this looks correct in isolation and wedges the
     * kernel the moment interrupts are enabled. */
    v->avail.flags = 1;

    uint16_t qs = virtio_pci_setup_queue(&v->vp, 0, VQ_SIZE,
                                         v->desc, (void *)&v->avail,
                                         (void *)&v->used, &v->notify_off);
    if (qs == 0) { kprintf("virtio-blk: queue 0 has size 0\n"); return false; }
    v->qsize = qs;

    virtio_pci_driver_ok(&v->vp);

    /* Capacity is the first field of the device-specific config, in sectors. */
    v->capacity = vr64(v->devcfg, 0);
    v->up = true;

    v->blkdev.block_count        = v->capacity;
    v->blkdev.block_size         = SECTOR;
    v->blkdev.read               = vblk_read;
    v->blkdev.write              = vblk_write;
    v->blkdev.flush              = vblk_flush;
    v->blkdev.driver_data        = v;
    v->blkdev.dma_max_phys       = UINT64_MAX;
    v->blkdev.needs_kernel_range = true;

    if (embk_block_register(&v->blkdev) != 0) {
        kprintf("virtio-blk: block layer refused registration\n");
        v->up = false;
        return false;
    }
    kprintf("virtio-blk: %s, %d sectors (%d MiB), queue %d, polled\n",
            v->blkdev.name, (int)v->capacity, (int)(v->capacity / 2048), (int)qs);
    return true;
}

bool virtio_blk_init(void) {
    /* EVERY matching function, not the first: the second disk on the machine
     * is the swap store or the crash test's seed, and was silently ignored
     * while this loop stopped at one. Registration order is PCI scan order,
     * so the first is sda -- the boot disk, on every configuration the
     * harness makes. */
    for (uint32_t i = 0; i < pci_devices_count() && g_nvblk < VBLK_MAX; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->vendor_id != VIRTIO_VENDOR ||
            (d->device_id != VIRTIO_BLK_DEVID_T && d->device_id != VIRTIO_BLK_DEVID_M))
            continue;
        if (vblk_attach(&g_vblks[g_nvblk], d))
            g_nvblk++;
    }
    if (g_nvblk == 0) {
        kprintf("virtio-blk: no device\n");
        return false;
    }
    return true;
}
