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

/* Rings live in .bss so KV2P() can turn them into the physical addresses the
 * device needs; a kmalloc'd ring would be in the heap window, which is mapped
 * but not KV2P-able. Alignment is the virtio spec's. */
static struct vring_desc  g_desc[VQ_SIZE]  __attribute__((aligned(16)));
static struct vring_avail g_avail          __attribute__((aligned(2)));
static struct vring_used  g_used           __attribute__((aligned(4)));

/* One bounce buffer: callers hand us kernel pointers that may not be
 * physically contiguous past a page, and a descriptor addresses physical
 * memory. 64 KiB matches the filesystem's largest read. */
#define BOUNCE_SZ (64u * 1024u)
static uint8_t g_bounce[BOUNCE_SZ] __attribute__((aligned(4096)));
static struct vblk_req_hdr g_hdr   __attribute__((aligned(16)));
static volatile uint8_t    g_status __attribute__((aligned(16)));

static struct {
    struct virtio_pci_dev vp;   /* the shared transport: windows + handshake */
    volatile uint8_t *common, *notify, *devcfg;
    uint32_t notify_multiplier;
    uint16_t notify_off;
    uint16_t qsize;
    uint16_t last_used;
    uint64_t capacity;          /* in 512-byte sectors */
    bool     up;
} g_vblk;

static struct embk_block_device g_blkdev;

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

/* Submit one three-descriptor request and spin until the device retires it. */
static int vblk_request(uint32_t type, uint64_t sector, void *data, uint32_t len,
                        bool device_writes) {
    if (!g_vblk.up)
        return -EMBK_EIO;

    g_hdr.type     = type;
    g_hdr.reserved = 0;
    g_hdr.sector   = sector;
    g_status       = 0xFF;          /* so "unchanged" is distinguishable */

    g_desc[0].addr  = dma(&g_hdr);
    g_desc[0].len   = sizeof(g_hdr);
    g_desc[0].flags = VRING_DESC_F_NEXT;
    g_desc[0].next  = 1;

    g_desc[1].addr  = dma(data);
    g_desc[1].len   = len;
    g_desc[1].flags = VRING_DESC_F_NEXT | (device_writes ? VRING_DESC_F_WRITE : 0);
    g_desc[1].next  = 2;

    g_desc[2].addr  = dma(&g_status);
    g_desc[2].len   = 1;
    g_desc[2].flags = VRING_DESC_F_WRITE;
    g_desc[2].next  = 0;

    uint16_t slot = g_avail.idx % g_vblk.qsize;
    g_avail.ring[slot] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();

    vw16(g_vblk.notify, (uint32_t)g_vblk.notify_off * g_vblk.notify_multiplier, 0);

    /* Spin for the completion. The bound is generous and exists only so a
     * device that never answers produces an error instead of a hung kernel --
     * a wedged disk must not be indistinguishable from a wedged machine. */
    for (uint64_t spins = 0; spins < 200000000ULL; spins++) {
        __sync_synchronize();
        if (g_used.idx != g_vblk.last_used) {
            g_vblk.last_used = g_used.idx;
            __sync_synchronize();
            return (g_status == 0) ? EMBK_OK : -EMBK_EIO;
        }
        arch_cpu_relax();
    }

    kprintf("virtio-blk: request timed out (type %d sector %d)\n",
            (int)type, (int)sector);
    return -EMBK_EIO;
}

static int vblk_read(struct embk_block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    (void)dev;
    uint8_t *out = (uint8_t *)buf;

    while (count) {
        uint32_t chunk = count > (BOUNCE_SZ / SECTOR) ? (BOUNCE_SZ / SECTOR) : count;
        int rc = vblk_request(VIRTIO_BLK_T_IN, lba, g_bounce, chunk * SECTOR, true);
        if (rc != EMBK_OK)
            return rc;
        memcpy(out, g_bounce, chunk * SECTOR);
        out   += chunk * SECTOR;
        lba   += chunk;
        count -= chunk;
    }
    return EMBK_OK;
}

static int vblk_write(struct embk_block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    (void)dev;
    const uint8_t *in = (const uint8_t *)buf;

    while (count) {
        uint32_t chunk = count > (BOUNCE_SZ / SECTOR) ? (BOUNCE_SZ / SECTOR) : count;
        memcpy(g_bounce, in, chunk * SECTOR);
        int rc = vblk_request(VIRTIO_BLK_T_OUT, lba, g_bounce, chunk * SECTOR, false);
        if (rc != EMBK_OK)
            return rc;
        in    += chunk * SECTOR;
        lba   += chunk;
        count -= chunk;
    }
    return EMBK_OK;
}

static int vblk_flush(struct embk_block_device *dev) {
    (void)dev;
    return vblk_request(VIRTIO_BLK_T_FLUSH, 0, g_bounce, 0, false);
}

bool virtio_blk_init(void) {
    const struct pci_device *dev = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d && d->vendor_id == VIRTIO_VENDOR &&
            (d->device_id == VIRTIO_BLK_DEVID_T || d->device_id == VIRTIO_BLK_DEVID_M)) {
            dev = d;
            break;
        }
    }
    if (!dev) {
        kprintf("virtio-blk: no device\n");
        return false;
    }

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
    if (!virtio_pci_attach(&g_vblk.vp, dev, "virtio-blk", 0, 0))
        return false;

    g_vblk.common = g_vblk.vp.common;
    g_vblk.notify = g_vblk.vp.notify;
    g_vblk.devcfg = g_vblk.vp.devcfg;
    g_vblk.notify_multiplier = g_vblk.vp.notify_multiplier;

    /* This driver DOES need the device config: capacity is its first field. */
    if (!g_vblk.devcfg) {
        kprintf("virtio-blk: device exposes no device-config window\n");
        return false;
    }

    /* --- queue 0 ------------------------------------------------------------ */
    memset(g_desc, 0, sizeof(g_desc));
    memset((void *)&g_avail, 0, sizeof(g_avail));
    memset((void *)&g_used, 0, sizeof(g_used));
    g_vblk.last_used = 0;

    /* VRING_AVAIL_F_NO_INTERRUPT. This driver POLLS, so it must tell the
     * device not to signal completions -- otherwise the device raises its
     * legacy INTx line on every request and, with no handler to clear a
     * LEVEL-triggered interrupt, the machine stops making progress. A polled
     * driver that forgets this looks correct in isolation and wedges the
     * kernel the moment interrupts are enabled. */
    g_avail.flags = 1;

    uint16_t qs = virtio_pci_setup_queue(&g_vblk.vp, 0, VQ_SIZE,
                                         g_desc, (void *)&g_avail,
                                         (void *)&g_used, &g_vblk.notify_off);
    if (qs == 0) { kprintf("virtio-blk: queue 0 has size 0\n"); return false; }
    g_vblk.qsize = qs;

    virtio_pci_driver_ok(&g_vblk.vp);

    /* Capacity is the first field of the device-specific config, in sectors. */
    g_vblk.capacity = vr64(g_vblk.devcfg, 0);
    g_vblk.up = true;

    g_blkdev.block_count      = g_vblk.capacity;
    g_blkdev.block_size       = SECTOR;
    g_blkdev.read             = vblk_read;
    g_blkdev.write            = vblk_write;
    g_blkdev.flush            = vblk_flush;
    g_blkdev.driver_data      = 0;
    g_blkdev.dma_max_phys     = UINT64_MAX;
    g_blkdev.needs_kernel_range = true;

    if (embk_block_register(&g_blkdev) != 0) {
        kprintf("virtio-blk: block layer refused registration\n");
        g_vblk.up = false;
        return false;
    }

    kprintf("virtio-blk: %s, %d sectors (%d MiB), queue %d, polled\n",
            g_blkdev.name, (int)g_vblk.capacity,
            (int)(g_vblk.capacity / 2048), (int)qs);
    return true;
}
