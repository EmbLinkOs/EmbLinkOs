/* virtio-net (modern, virtio 1.x PCI) -- the M1 NIC driver.
 *
 * Mirrors virtio_gpu.c's transport bring-up (vendor caps -> common/notify/device
 * config windows, split virtqueues, KV2P DMA) but drives TWO queues: the
 * receiveq (0) and the transmitq (1). RX is polled: virtio_net_poll() drains the
 * used ring into net_rx() and re-posts each buffer; a kthread calls it in a
 * loop. TX is synchronous under a lock (post one [virtio_net_hdr | frame]
 * descriptor, notify, wait for the used entry) -- enough for M1, where the only
 * senders are the RX thread (echo replies) and the `test net` witness.
 *
 * Every buffer and ring lives in the kernel image's .bss, which is physically
 * contiguous (kernel vaddr -> phys is a fixed linear offset, so KV2P is exact
 * and no DMA buffer straddles a discontiguity). */

#include "net/net.h"
#include "drivers/bus/pci.h"
#include "drivers/char/serial.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "arch/x86_64/cpu/spinlock.h"

/* ---- virtio PCI capability + common-config layout (as virtio_gpu.c) ------ */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VC_DEVICE_FEATURE_SELECT 0x00
#define VC_DEVICE_FEATURE        0x04
#define VC_DRIVER_FEATURE_SELECT 0x08
#define VC_DRIVER_FEATURE        0x0C
#define VC_NUM_QUEUES            0x12
#define VC_DEVICE_STATUS         0x14
#define VC_QUEUE_SELECT          0x16
#define VC_QUEUE_SIZE            0x18
#define VC_QUEUE_ENABLE          0x1C
#define VC_QUEUE_NOTIFY_OFF      0x1E
#define VC_QUEUE_DESC            0x20
#define VC_QUEUE_DRIVER          0x28
#define VC_QUEUE_DEVICE          0x30

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8

#define VIRTIO_F_VERSION_1_BANK 1
#define VIRTIO_F_VERSION_1_BIT  (1U << 0)   /* feature bit 32 = bank 1 bit 0 */
#define VIRTIO_NET_F_MAC_BIT    (1U << 5)   /* feature bit 5 = bank 0 bit 5 */

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

#define VQ_SIZE   64
#define RX_BUFS   32                       /* receive buffers posted at once */
#define NET_BUF_SZ 2048                    /* >= 12 (hdr) + 1514 (frame) */

struct vring_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vring_avail { uint16_t flags; uint16_t idx; uint16_t ring[VQ_SIZE]; } __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct vring_used { uint16_t flags; uint16_t idx; struct vring_used_elem ring[VQ_SIZE]; } __attribute__((packed));

/* virtio 1.0 net header -- 12 bytes (num_buffers always present). Prepended to
 * every frame on TX; the device prepends it on RX and we skip it. */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} __attribute__((packed));

struct virtq {
    uint16_t size;
    uint16_t notify_off;
    uint16_t last_used_idx;
    struct vring_desc  desc[VQ_SIZE]  __attribute__((aligned(4096)));
    struct vring_avail avail          __attribute__((aligned(4096)));
    struct vring_used  used           __attribute__((aligned(4096)));
};

struct vnet_state {
    bool present;
    volatile uint8_t *common;
    volatile uint8_t *notify_base;
    volatile uint8_t *device_cfg;
    uint32_t notify_off_multiplier;

    struct virtq rxq;
    struct virtq txq;

    uint8_t rx_buf[RX_BUFS][NET_BUF_SZ] __attribute__((aligned(64)));
    uint8_t tx_buf[NET_BUF_SZ]          __attribute__((aligned(64)));
};

static struct vnet_state g_vnet;
static spinlock_t vnet_tx_lock = SPINLOCK_INIT;
static spinlock_t vnet_rx_lock = SPINLOCK_INIT;

/* ---- MMIO helpers (as virtio_gpu.c) ------------------------------------- */
static inline uint8_t  vr8 (volatile uint8_t *b, uint32_t o) { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t vr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void vw8 (volatile uint8_t *b, uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(b + o) = v; }
static inline void vw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void vw64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    vw32(b, o, (uint32_t)v); vw32(b, o + 4, (uint32_t)(v >> 32));
}
static inline uint64_t vnet_dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static void vnet_notify(struct virtq *q, uint16_t queue_index) {
    vw16((volatile uint8_t *)g_vnet.notify_base,
         (uint32_t)q->notify_off * g_vnet.notify_off_multiplier, queue_index);
}

static volatile uint8_t *vnet_map_cap(const struct pci_device *dev, uint8_t bar,
                                      uint32_t offset, uint32_t length) {
    struct pci_bar b = pci_read_bar(dev->bus, dev->device, dev->function, bar);
    if (!b.valid || !b.is_mmio) return 0;
    return (volatile uint8_t *)vmm_map_mmio(b.address + offset, length ? length : 4096);
}

/* Program one virtqueue's desc/avail/used addresses into the common config. */
static bool vnet_setup_queue(uint16_t idx, struct virtq *q) {
    volatile uint8_t *c = g_vnet.common;
    vw16(c, VC_QUEUE_SELECT, idx);
    uint16_t qs = vr16(c, VC_QUEUE_SIZE);
    if (qs == 0) return false;
    if (qs > VQ_SIZE) { vw16(c, VC_QUEUE_SIZE, VQ_SIZE); qs = VQ_SIZE; }
    q->size = qs;
    q->last_used_idx = 0;
    memset(q->desc, 0, sizeof(q->desc));
    memset((void *)&q->avail, 0, sizeof(q->avail));
    memset((void *)&q->used, 0, sizeof(q->used));
    vw64(c, VC_QUEUE_DESC,   vnet_dma(q->desc));
    vw64(c, VC_QUEUE_DRIVER, vnet_dma(&q->avail));
    vw64(c, VC_QUEUE_DEVICE, vnet_dma(&q->used));
    q->notify_off = vr16(c, VC_QUEUE_NOTIFY_OFF);
    vw16(c, VC_QUEUE_ENABLE, 1);
    return true;
}

/* Post RX buffer `i` as a single device-writable descriptor and make it
 * available. Caller holds vnet_rx_lock. */
static void vnet_rx_post(uint16_t i) {
    struct virtq *q = &g_vnet.rxq;
    q->desc[i].addr  = vnet_dma(g_vnet.rx_buf[i]);
    q->desc[i].len   = NET_BUF_SZ;
    q->desc[i].flags = VRING_DESC_F_WRITE;
    q->desc[i].next  = 0;
    uint16_t slot = q->avail.idx % q->size;
    q->avail.ring[slot] = i;
    __sync_synchronize();
    q->avail.idx++;
    __sync_synchronize();
}

static bool vnet_init_transport(const struct pci_device *dev) {
    struct vnet_state *s = &g_vnet;

    uint16_t status = pci_read16(dev->bus, dev->device, dev->function, PCI_STATUS);
    if (!(status & (1 << 4))) { kprintf("virtio-net: no PCI capability list\n"); return false; }

    uint8_t cap = pci_read8(dev->bus, dev->device, dev->function, PCI_CAP_PTR) & 0xFC;
    uint8_t common_bar = 0xFF, notify_bar = 0xFF, dev_bar = 0xFF;
    uint32_t common_off = 0, common_len = 0, notify_off = 0, notify_len = 0, dev_off = 0, dev_len = 0;

    while (cap) {
        uint8_t id  = pci_read8(dev->bus, dev->device, dev->function, cap);
        uint8_t nxt = pci_read8(dev->bus, dev->device, dev->function, cap + 1);
        if (id == 0x09) {
            uint8_t type   = pci_read8 (dev->bus, dev->device, dev->function, cap + 3);
            uint8_t bar    = pci_read8 (dev->bus, dev->device, dev->function, cap + 4);
            uint32_t off   = pci_read32(dev->bus, dev->device, dev->function, cap + 8);
            uint32_t len   = pci_read32(dev->bus, dev->device, dev->function, cap + 12);
            if (type == VIRTIO_PCI_CAP_COMMON_CFG && common_bar == 0xFF) {
                common_bar = bar; common_off = off; common_len = len;
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG && notify_bar == 0xFF) {
                notify_bar = bar; notify_off = off; notify_len = len;
                s->notify_off_multiplier = pci_read32(dev->bus, dev->device, dev->function, cap + 16);
            } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG && dev_bar == 0xFF) {
                dev_bar = bar; dev_off = off; dev_len = len;
            }
        }
        cap = nxt & 0xFC;
    }
    if (common_bar == 0xFF || notify_bar == 0xFF || dev_bar == 0xFF) {
        kprintf("virtio-net: missing common/notify/device capability\n"); return false;
    }

    s->common     = vnet_map_cap(dev, common_bar, common_off, common_len);
    s->notify_base= vnet_map_cap(dev, notify_bar, notify_off, notify_len);
    s->device_cfg = vnet_map_cap(dev, dev_bar,    dev_off,    dev_len);
    if (!s->common || !s->notify_base || !s->device_cfg) {
        kprintf("virtio-net: failed to map config windows\n"); return false;
    }

    pci_enable_bus_mastering(dev->bus, dev->device, dev->function);

    volatile uint8_t *c = s->common;
    vw8(c, VC_DEVICE_STATUS, 0);
    for (uint32_t i = 0; i < 1000000 && vr8(c, VC_DEVICE_STATUS) != 0; i++) { }
    vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vw8(c, VC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Need VERSION_1 (bank 1 bit 0) + MAC (bank 0 bit 5). */
    vw32(c, VC_DEVICE_FEATURE_SELECT, VIRTIO_F_VERSION_1_BANK);
    if (!(vr32(c, VC_DEVICE_FEATURE) & VIRTIO_F_VERSION_1_BIT)) {
        kprintf("virtio-net: device does not offer VERSION_1\n"); return false;
    }
    vw32(c, VC_DEVICE_FEATURE_SELECT, 0);
    uint32_t feat0 = vr32(c, VC_DEVICE_FEATURE);
    /* Accept MAC if offered; ask for nothing else (no checksum offload, no
     * mergeable buffers -- keeps the RX path a plain 1 buffer = 1 frame). */
    vw32(c, VC_DRIVER_FEATURE_SELECT, 0);
    vw32(c, VC_DRIVER_FEATURE, feat0 & VIRTIO_NET_F_MAC_BIT);
    vw32(c, VC_DRIVER_FEATURE_SELECT, VIRTIO_F_VERSION_1_BANK);
    vw32(c, VC_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT);

    uint8_t st = vr8(c, VC_DEVICE_STATUS) | VIRTIO_STATUS_FEATURES_OK;
    vw8(c, VC_DEVICE_STATUS, st);
    if (!(vr8(c, VC_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("virtio-net: FEATURES_OK rejected\n"); return false;
    }

    if (!vnet_setup_queue(0, &s->rxq) || !vnet_setup_queue(1, &s->txq)) {
        kprintf("virtio-net: queue setup failed\n"); return false;
    }

    /* Fill the receive ring before going live. */
    for (uint16_t i = 0; i < RX_BUFS && i < s->rxq.size; i++) vnet_rx_post(i);

    vw8(c, VC_DEVICE_STATUS, vr8(c, VC_DEVICE_STATUS) | VIRTIO_STATUS_DRIVER_OK);
    vnet_notify(&s->rxq, 0);
    return true;
}

bool virtio_net_init(uint8_t mac_out[ETH_ALEN]) {
    /* Find the virtio-net PCI function: vendor 0x1AF4, device 0x1000 (legacy id)
     * or 0x1041 (modern id); network class 0x02. */
    const struct pci_device *nic = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d->vendor_id == 0x1AF4 &&
            (d->device_id == 0x1000 || d->device_id == 0x1041)) { nic = d; break; }
    }
    if (!nic) { kprintf("virtio-net: no device found\n"); return false; }

    if (!vnet_init_transport(nic)) return false;

    for (int i = 0; i < ETH_ALEN; i++) mac_out[i] = vr8(g_vnet.device_cfg, i);
    g_vnet.present = true;
    kprintf("virtio-net: up, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4], mac_out[5]);
    return true;
}

int virtio_net_tx(const void *frame, uint32_t len) {
    if (!g_vnet.present || len > ETH_FRAME_MAX) return -1;
    struct virtq *q = &g_vnet.txq;
    int rc = -1;

    spin_lock(&vnet_tx_lock);

    struct virtio_net_hdr *h = (struct virtio_net_hdr *)g_vnet.tx_buf;
    memset(h, 0, sizeof(*h));
    memcpy(g_vnet.tx_buf + sizeof(*h), frame, len);

    q->desc[0].addr  = vnet_dma(g_vnet.tx_buf);
    q->desc[0].len   = sizeof(*h) + len;
    q->desc[0].flags = 0;                       /* device-readable */
    q->desc[0].next  = 0;

    uint16_t slot = q->avail.idx % q->size;
    q->avail.ring[slot] = 0;
    __sync_synchronize();
    q->avail.idx++;
    __sync_synchronize();
    vnet_notify(q, 1);

    for (uint32_t spins = 0; spins < 200000000U; spins++) {
        __sync_synchronize();
        if (q->used.idx != q->last_used_idx) { q->last_used_idx = q->used.idx; rc = (int)len; break; }
    }

    spin_unlock(&vnet_tx_lock);
    if (rc < 0) kprintf("virtio-net: TX timeout\n");
    return rc;
}

void virtio_net_poll(void) {
    if (!g_vnet.present) return;
    struct virtq *q = &g_vnet.rxq;

    for (;;) {
        spin_lock(&vnet_rx_lock);
        __sync_synchronize();
        if (q->used.idx == q->last_used_idx) { spin_unlock(&vnet_rx_lock); return; }

        struct vring_used_elem e = q->used.ring[q->last_used_idx % q->size];
        q->last_used_idx++;
        uint16_t i = (uint16_t)e.id;
        uint32_t total = e.len;

        /* Copy out of the DMA buffer before re-posting it, so net_rx() can run
         * without holding the RX lock (and can itself TX a reply). */
        static uint8_t frame[NET_BUF_SZ];
        uint32_t flen = 0;
        if (i < RX_BUFS && total > sizeof(struct virtio_net_hdr)) {
            flen = total - sizeof(struct virtio_net_hdr);
            if (flen > sizeof(frame)) flen = sizeof(frame);
            memcpy(frame, g_vnet.rx_buf[i] + sizeof(struct virtio_net_hdr), flen);
        }
        if (i < RX_BUFS) vnet_rx_post(i);
        vnet_notify(q, 0);
        spin_unlock(&vnet_rx_lock);

        if (flen) net_rx(frame, flen);
    }
}
