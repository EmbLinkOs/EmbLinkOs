#include "drivers/bus/virtio_pci.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "mm/pmm.h"          /* KV2P -- the shared kernel-virtual to physical map */

/* The shared virtio-over-PCI modern transport. See virtio_pci.h for why this
 * file exists and what it deliberately does not own. */

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_F_VERSION_1_BIT (1U << 0)   /* feature bit 32 -> bank 1, bit 0 */

const struct pci_device *virtio_pci_find(uint16_t device_id, uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *d = pci_get_device(i);
        if (d && d->vendor_id == VIRTIO_VENDOR_ID && d->device_id == device_id) {
            if (seen == index)
                return d;
            seen++;
        }
    }
    return 0;
}

static volatile uint8_t *map_cap(const struct pci_device *d, uint8_t bar,
                                 uint32_t off, uint32_t len) {
    struct pci_bar b = pci_read_bar(d->bus, d->device, d->function, bar);
    if (!b.valid || !b.is_mmio)
        return 0;
    return (volatile uint8_t *)(uintptr_t)vmm_map_mmio(b.address + off,
                                                       len ? len : 4096);
}

bool virtio_pci_attach(struct virtio_pci_dev *vd, const struct pci_device *pci,
                       const char *name)
{
    vd->pci = pci;
    vd->common = vd->notify = vd->devcfg = 0;
    vd->notify_multiplier = 0;

    uint16_t status = pci_read16(pci->bus, pci->device, pci->function, PCI_STATUS);
    if (!(status & (1u << 4))) {
        kprintf("%s: no capability list\n", name);
        return false;
    }

    uint8_t cap = pci_read8(pci->bus, pci->device, pci->function, PCI_CAP_PTR) & 0xFC;
    uint8_t cbar = 0xFF, nbar = 0xFF, dbar = 0xFF;
    uint32_t coff = 0, clen = 0, noff = 0, nlen = 0, doff = 0, dlen = 0;

    /* Bounded walk: a malformed or hostile capability list that points at
     * itself would otherwise spin here forever. */
    for (int guard = 0; cap && guard < 48; guard++) {
        uint8_t id  = pci_read8(pci->bus, pci->device, pci->function, cap);
        uint8_t nxt = pci_read8(pci->bus, pci->device, pci->function, cap + 1);
        if (id == 0x09) {                       /* PCI_CAP_ID_VNDR */
            uint8_t  t   = pci_read8 (pci->bus, pci->device, pci->function, cap + 3);
            uint8_t  bar = pci_read8 (pci->bus, pci->device, pci->function, cap + 4);
            uint32_t off = pci_read32(pci->bus, pci->device, pci->function, cap + 8);
            uint32_t len = pci_read32(pci->bus, pci->device, pci->function, cap + 12);
            if (t == VIRTIO_PCI_CAP_COMMON_CFG && cbar == 0xFF) {
                cbar = bar; coff = off; clen = len;
            } else if (t == VIRTIO_PCI_CAP_NOTIFY_CFG && nbar == 0xFF) {
                nbar = bar; noff = off; nlen = len;
                vd->notify_multiplier =
                    pci_read32(pci->bus, pci->device, pci->function, cap + 16);
            } else if (t == VIRTIO_PCI_CAP_DEVICE_CFG && dbar == 0xFF) {
                dbar = bar; doff = off; dlen = len;
            }
        }
        cap = nxt & 0xFC;
    }
    if (cbar == 0xFF || nbar == 0xFF || dbar == 0xFF) {
        kprintf("%s: missing a required capability\n", name);
        return false;
    }

    vd->common = map_cap(pci, cbar, coff, clen);
    vd->notify = map_cap(pci, nbar, noff, nlen);
    vd->devcfg = map_cap(pci, dbar, doff, dlen);
    if (!vd->common || !vd->notify || !vd->devcfg) {
        kprintf("%s: could not map config windows\n", name);
        return false;
    }

    /* Without bus mastering a virtio device cannot complete a single DMA, and
     * there is no firmware on `virt` to have enabled it. */
    pci_enable_bus_mastering(pci->bus, pci->device, pci->function);

    volatile uint8_t *c = vd->common;
    vp_w8(c, VP_DEVICE_STATUS, 0);                      /* reset */
    while (vp_r8(c, VP_DEVICE_STATUS) != 0) { }
    vp_w8(c, VP_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vp_w8(c, VP_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Exactly VIRTIO_F_VERSION_1 and nothing else -- the same choice
     * virtio_blk.c documents: every optional feature adds an obligation, and
     * negotiating nothing is the version that cannot be subtly wrong. */
    vp_w32(c, VP_DRIVER_FEATURE_SELECT, 0); vp_w32(c, VP_DRIVER_FEATURE, 0);
    vp_w32(c, VP_DRIVER_FEATURE_SELECT, 1);
    vp_w32(c, VP_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT);

    vp_w8(c, VP_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                               VIRTIO_STATUS_FEATURES_OK);
    if (!(vp_r8(c, VP_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("%s: device refused VIRTIO_F_VERSION_1\n", name);
        vp_w8(c, VP_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    return true;
}

uint16_t virtio_pci_setup_queue(struct virtio_pci_dev *vd, uint16_t q,
                                uint16_t want, void *desc, void *avail,
                                void *used, uint16_t *notify_off)
{
    volatile uint8_t *c = vd->common;

    vp_w16(c, VP_QUEUE_SELECT, q);
    uint16_t qs = vp_r16(c, VP_QUEUE_SIZE);
    if (qs == 0)
        return 0;                                  /* no such queue */
    if (qs > want) { vp_w16(c, VP_QUEUE_SIZE, want); qs = want; }

    vp_w64(c, VP_QUEUE_DESC,   KV2P((uint64_t)(uintptr_t)desc));
    vp_w64(c, VP_QUEUE_DRIVER, KV2P((uint64_t)(uintptr_t)avail));
    vp_w64(c, VP_QUEUE_DEVICE, KV2P((uint64_t)(uintptr_t)used));
    vp_w16(c, VP_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    if (notify_off)
        *notify_off = vp_r16(c, VP_QUEUE_NOTIFY_OFF);
    vp_w16(c, VP_QUEUE_ENABLE, 1);
    return qs;
}

void virtio_pci_driver_ok(struct virtio_pci_dev *vd) {
    vp_w8(vd->common, VP_DEVICE_STATUS,
          VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
          VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

void virtio_pci_notify(struct virtio_pci_dev *vd, uint16_t notify_off, uint16_t q) {
    vp_w16(vd->notify, (uint32_t)notify_off * vd->notify_multiplier, q);
}
