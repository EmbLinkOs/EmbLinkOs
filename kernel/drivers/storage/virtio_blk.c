#include "drivers/storage/virtio_blk.h"
#include "drivers/bus/pci.h"
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

struct vring_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vring_avail { uint16_t flags; uint16_t idx; uint16_t ring[VQ_SIZE]; } __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
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

static volatile uint8_t *map_cap(const struct pci_device *d, uint8_t bar,
                                 uint32_t off, uint32_t len) {
    struct pci_bar b = pci_read_bar(d->bus, d->device, d->function, bar);
    if (!b.valid || !b.is_mmio)
        return 0;
    return (volatile uint8_t *)(uintptr_t)vmm_map_mmio(b.address + off, len ? len : 4096);
}

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

    /* --- capability walk --------------------------------------------------- */
    uint16_t status = pci_read16(dev->bus, dev->device, dev->function, PCI_STATUS);
    if (!(status & (1u << 4))) { kprintf("virtio-blk: no capability list\n"); return false; }

    uint8_t cap = pci_read8(dev->bus, dev->device, dev->function, PCI_CAP_PTR) & 0xFC;
    uint8_t cbar = 0xFF, nbar = 0xFF, dbar = 0xFF;
    uint32_t coff = 0, clen = 0, noff = 0, nlen = 0, doff = 0, dlen = 0;

    for (int guard = 0; cap && guard < 48; guard++) {
        uint8_t id  = pci_read8(dev->bus, dev->device, dev->function, cap);
        uint8_t nxt = pci_read8(dev->bus, dev->device, dev->function, cap + 1);
        if (id == 0x09) {
            uint8_t  t   = pci_read8 (dev->bus, dev->device, dev->function, cap + 3);
            uint8_t  bar = pci_read8 (dev->bus, dev->device, dev->function, cap + 4);
            uint32_t off = pci_read32(dev->bus, dev->device, dev->function, cap + 8);
            uint32_t len = pci_read32(dev->bus, dev->device, dev->function, cap + 12);
            if (t == VIRTIO_PCI_CAP_COMMON_CFG && cbar == 0xFF) { cbar = bar; coff = off; clen = len; }
            else if (t == VIRTIO_PCI_CAP_NOTIFY_CFG && nbar == 0xFF) {
                nbar = bar; noff = off; nlen = len;
                g_vblk.notify_multiplier = pci_read32(dev->bus, dev->device, dev->function, cap + 16);
            }
            else if (t == VIRTIO_PCI_CAP_DEVICE_CFG && dbar == 0xFF) { dbar = bar; doff = off; dlen = len; }
        }
        cap = nxt & 0xFC;
    }
    if (cbar == 0xFF || nbar == 0xFF || dbar == 0xFF) {
        kprintf("virtio-blk: missing a required capability\n"); return false;
    }

    g_vblk.common = map_cap(dev, cbar, coff, clen);
    g_vblk.notify = map_cap(dev, nbar, noff, nlen);
    g_vblk.devcfg = map_cap(dev, dbar, doff, dlen);
    if (!g_vblk.common || !g_vblk.notify || !g_vblk.devcfg) {
        kprintf("virtio-blk: could not map config windows\n"); return false;
    }

    pci_enable_bus_mastering(dev->bus, dev->device, dev->function);

    /* --- the virtio handshake ---------------------------------------------- */
    volatile uint8_t *c = g_vblk.common;
    vw8(c, VC_DEVICE_STATUS, 0);                       /* reset */
    while (vr8(c, VC_DEVICE_STATUS) != 0) { }
    vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Accept exactly VIRTIO_F_VERSION_1 and nothing else: every optional
     * feature (segment limits, discard, multi-queue) changes the request
     * format or adds obligations, and none of them is needed to read a
     * sector. Negotiating nothing is the version that cannot be subtly wrong. */
    vw32(c, VC_DRIVER_FEATURE_SELECT, 0); vw32(c, VC_DRIVER_FEATURE, 0);
    vw32(c, VC_DRIVER_FEATURE_SELECT, 1); vw32(c, VC_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT);

    vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK);
    if (!(vr8(c, VC_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("virtio-blk: device refused VIRTIO_F_VERSION_1\n");
        vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }

    /* --- queue 0 ------------------------------------------------------------ */
    vw16(c, VC_QUEUE_SELECT, 0);
    uint16_t qs = vr16(c, VC_QUEUE_SIZE);
    if (qs == 0) { kprintf("virtio-blk: queue 0 has size 0\n"); return false; }
    if (qs > VQ_SIZE) { vw16(c, VC_QUEUE_SIZE, VQ_SIZE); qs = VQ_SIZE; }
    g_vblk.qsize = qs;

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

    vw64(c, VC_QUEUE_DESC,   dma(g_desc));
    vw64(c, VC_QUEUE_DRIVER, dma(&g_avail));
    vw64(c, VC_QUEUE_DEVICE, dma(&g_used));
    vw16(c, VC_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);   /* polled */
    g_vblk.notify_off = vr16(c, VC_QUEUE_NOTIFY_OFF);
    vw16(c, VC_QUEUE_ENABLE, 1);

    vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

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
