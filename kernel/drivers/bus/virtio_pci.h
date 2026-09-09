#ifndef _VIRTIO_PCI_H_
#define _VIRTIO_PCI_H_

#include <stdint.h>
#include "include/types.h"   /* bool -- NOT <stdbool.h>: types.h typedefs it */
#include "drivers/bus/pci.h"

/* The virtio-over-PCI MODERN transport, factored out.
 *
 * docs/TODO.md: "The virtio-PCI capability walk is now written THREE times
 * (virtio_net.c, virtio_gpu.c, virtio_blk.c). Factor it into a shared
 * virtio-pci transport when a fourth appears, not before." virtio-input is the
 * fourth, so this is that file -- written instead of the fourth copy rather
 * than after it.
 *
 * The three EXISTING drivers still carry their own copies and should migrate to
 * this (docs/TODO.md); nothing here changes their behaviour, and folding a
 * refactor of three working drivers into an ARM bring-up would have made any
 * regression ambiguous.
 *
 * What this owns: finding the device, walking the vendor capabilities, mapping
 * the three config windows, the status handshake, and one virtqueue's
 * registration. What it does NOT own: the ring memory (the caller supplies it,
 * because a ring must live where KV2P() works -- see the note in virtio_blk.c)
 * and everything device-specific. */

/* Common-configuration register offsets (virtio 1.x, 4.1.4.3). */
#define VP_DEVICE_FEATURE_SELECT 0x00
#define VP_DEVICE_FEATURE        0x04
#define VP_DRIVER_FEATURE_SELECT 0x08
#define VP_DRIVER_FEATURE        0x0C
#define VP_MSIX_CONFIG           0x10
#define VP_NUM_QUEUES            0x12
#define VP_DEVICE_STATUS         0x14
#define VP_QUEUE_SELECT          0x16
#define VP_QUEUE_SIZE            0x18
#define VP_QUEUE_MSIX_VECTOR     0x1A
#define VP_QUEUE_ENABLE          0x1C
#define VP_QUEUE_NOTIFY_OFF      0x1E
#define VP_QUEUE_DESC            0x20
#define VP_QUEUE_DRIVER          0x28
#define VP_QUEUE_DEVICE          0x30

#define VIRTIO_VENDOR_ID          0x1AF4

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_MSI_NO_VECTOR      0xFFFF

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2
#define VRING_AVAIL_F_NO_INTERRUPT 1

struct vring_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; }
    __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));

/* A mapped, handshaken device. */
struct virtio_pci_dev {
    const struct pci_device *pci;
    volatile uint8_t *common;      /* common configuration                */
    volatile uint8_t *notify;      /* notification window                 */
    volatile uint8_t *devcfg;      /* device-specific config, or NULL     */
    uint32_t notify_multiplier;
};

static inline uint8_t  vp_r8 (volatile uint8_t *b, uint32_t o) { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t vp_r16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vp_r32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void vp_w8 (volatile uint8_t *b, uint32_t o, uint8_t v)  { *(volatile uint8_t  *)(b + o) = v; }
static inline void vp_w16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vp_w32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void vp_w64(volatile uint8_t *b, uint32_t o, uint64_t v) {
    vp_w32(b, o, (uint32_t)v); vp_w32(b, o + 4, (uint32_t)(v >> 32));
}

/* Find the Nth device with this vendor/device id pair. `index` lets a caller
 * walk several identical devices -- virtio-input is the case that needs it: a
 * keyboard and a tablet are two devices with the SAME id, distinguishable only
 * by asking each what it is. Returns NULL when there is no such device. */
const struct pci_device *virtio_pci_find(uint16_t device_id, uint32_t index);

/* Walk the capabilities, map the three windows, reset the device and negotiate
 * features. `name` is used only for diagnostics. Leaves the device in
 * DRIVER|ACKNOWLEDGE|FEATURES_OK -- the caller sets up its queues and then
 * calls virtio_pci_driver_ok().
 *
 * VIRTIO_F_VERSION_1 (bank 1, bit 0) is always required: a device that does not
 * offer it is refused, because every one of these drivers is written against
 * the modern layout and nothing here implements the legacy one.
 *
 * `want0` is the bank-0 feature mask the caller would LIKE. What it gets is the
 * intersection with what the device offers, reported through `got0` (may be
 * null). Asking for zero is the common case and the safe one -- every optional
 * feature adds an obligation, and virtio-blk/gpu/input all want none of them.
 * virtio-net is the exception: it asks for VIRTIO_NET_F_MAC, because reading
 * the device's MAC out of config space is only valid if that was negotiated. */
bool virtio_pci_attach(struct virtio_pci_dev *vd, const struct pci_device *pci,
                       const char *name, uint32_t want0, uint32_t *got0);

/* Register one virtqueue. `desc`/`avail`/`used` are the caller's ring memory
 * (physical addresses are taken with KV2P, so it must be static storage, not
 * the heap). Returns the negotiated size, or 0 on failure; *notify_off receives
 * the queue's notification offset for virtio_pci_notify(). */
uint16_t virtio_pci_setup_queue(struct virtio_pci_dev *vd, uint16_t q,
                                uint16_t want, void *desc, void *avail,
                                void *used, uint16_t *notify_off);

void virtio_pci_driver_ok(struct virtio_pci_dev *vd);
void virtio_pci_notify(struct virtio_pci_dev *vd, uint16_t notify_off, uint16_t q);

#endif /* _VIRTIO_PCI_H_ */
