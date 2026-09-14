/* kernel/drivers/iommu/virtio_iommu.c -- the device that decides what a device
 * is allowed to reach.
 *
 * Every driver in this tree hands hardware a PHYSICAL ADDRESS and the hardware
 * reads or writes it. That is how DMA has worked here since the first ATA
 * transfer, and it means any device on the bus can read or write ANY byte of
 * memory -- the kernel's page tables, another process's pages, the disk
 * encryption key. A malicious Thunderbolt device plugged into a running laptop
 * needs no exploit; it just asks.
 *
 * An IOMMU is the page table for that. The device asks for an address, the
 * IOMMU translates it, and an address with no translation is refused instead
 * of served.
 *
 * WHICH MAKES THIS THE ONE DRIVER THAT CAN STOP THE MACHINE BY WORKING. The
 * moment an endpoint is attached to a domain, everything it does goes through
 * that domain's table -- and a table with nothing in it means a disk
 * controller that can no longer reach its own descriptors. So the order here
 * is: run before any DMA-capable driver has been initialised, attach every
 * endpoint, and install the mapping in the same breath.
 *
 * THE MAPPING IS AN IDENTITY ONE, over the memory the kernel actually uses,
 * and that is worth being honest about: it protects nothing yet. What it
 * buys is that the plumbing is real and exercised -- endpoints attached,
 * domains created, translations installed and honoured -- so that narrowing
 * it later is a change to WHICH ranges are mapped, not a new subsystem. A
 * per-driver map/unmap around each buffer is the thing that would actually
 * contain a hostile device, and it is recorded in docs/TODO.md as not done.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/iommu/virtio_iommu.h"
#include "mm/pmm.h"

#define VIRTIO_IOMMU_DEVID 0x1057   /* 0x1040 + device type 23 */

#define VI_T_ATTACH 0x01
#define VI_T_DETACH 0x02
#define VI_T_MAP    0x03
#define VI_T_UNMAP  0x04

#define VI_MAP_F_READ  (1u << 0)
#define VI_MAP_F_WRITE (1u << 1)

#define VI_S_OK 0

#define VI_QSIZE 8
#define VI_DOMAIN 1

/* Configuration space. */
#define VIC_PAGE_SIZE_MASK 0
#define VIC_INPUT_START    8
#define VIC_INPUT_END     16
#define VIC_DOMAIN_START  24
#define VIC_DOMAIN_END    28
#define VIC_PROBE_SIZE    32
#define VIC_BYPASS        36

struct vi_req_head { uint8_t type; uint8_t reserved[3]; } __attribute__((packed));
struct vi_req_tail { uint8_t status; uint8_t reserved[3]; } __attribute__((packed));

struct vi_req_attach {
    struct vi_req_head head;
    uint32_t domain;
    uint32_t endpoint;
    uint8_t  reserved[8];
} __attribute__((packed));

struct vi_req_map {
    struct vi_req_head head;
    uint32_t domain;
    uint64_t virt_start;
    uint64_t virt_end;
    uint64_t phys_start;
    uint32_t flags;
} __attribute__((packed));

static struct virtio_pci_dev g_vd;
static bool g_up;
static uint32_t g_attached;
static uint64_t g_mapped_bytes;

static struct vring_desc g_desc[VI_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[VI_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_avail;
static struct { uint16_t flags, idx; struct vring_used_elem ring[VI_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_used;
static uint8_t g_reqbuf[64] __attribute__((aligned(64)));
static struct vi_req_tail g_tail __attribute__((aligned(64)));
static uint16_t g_notify, g_qsize, g_last;

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* One request. Every virtio-iommu request is "a fixed-size structure in, a
 * one-byte status out", so there is one function for all of them. */
static bool vi_request(const void *req, uint32_t len) {
    if (len > sizeof g_reqbuf) return false;
    memcpy(g_reqbuf, req, len);
    g_tail.status = 0xFF;

    g_desc[0].addr = dma(g_reqbuf); g_desc[0].len = len;
    g_desc[0].flags = VRING_DESC_F_NEXT; g_desc[0].next = 1;
    g_desc[1].addr = dma(&g_tail); g_desc[1].len = sizeof g_tail;
    g_desc[1].flags = VRING_DESC_F_WRITE; g_desc[1].next = 0;

    g_avail.ring[g_avail.idx % g_qsize] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify, 0);

    for (int spin = 0; spin < 20000000; spin++) {
        if (g_used.idx != g_last) {
            g_last++;
            return g_tail.status == VI_S_OK;
        }
        __asm__ volatile("" ::: "memory");
    }
    kprintf("virtio-iommu: a request was never answered\n");
    return false;
}

static bool vi_attach(uint32_t endpoint) {
    struct vi_req_attach r;
    memset(&r, 0, sizeof r);
    r.head.type = VI_T_ATTACH;
    r.domain = VI_DOMAIN;
    r.endpoint = endpoint;
    return vi_request(&r, sizeof r);
}

static bool vi_map(uint64_t start, uint64_t end) {
    struct vi_req_map r;
    memset(&r, 0, sizeof r);
    r.head.type = VI_T_MAP;
    r.domain = VI_DOMAIN;
    r.virt_start = start;
    /* THE RANGE IS INCLUSIVE AT BOTH ENDS. An exclusive end leaves the last
     * page of every mapping untranslated, which shows up as a device that
     * works until a transfer happens to finish on a boundary. */
    r.virt_end = end;
    r.phys_start = start;
    r.flags = VI_MAP_F_READ | VI_MAP_F_WRITE;
    return vi_request(&r, sizeof r);
}

bool virtio_iommu_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_IOMMU_DEVID, 0);
    if (!pci) return false;
    if (!virtio_pci_attach(&g_vd, pci, "virtio-iommu", 0, 0)) return false;
    if (!g_vd.devcfg) {
        kprintf("virtio-iommu: no config window\n");
        return false;
    }

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)&g_avail, 0, sizeof g_avail);
    memset((void *)&g_used, 0, sizeof g_used);
    g_avail.flags = VRING_AVAIL_F_NO_INTERRUPT;
    /* Queue 0 is requests; queue 1 is the event queue, where translation
     * FAULTS are reported. Not configured: a fault here means a driver used
     * an address nobody mapped, and the failure is already loud (the transfer
     * does not happen). Reading the faults would name the device, which is
     * worth having and is recorded rather than half-done. */
    g_qsize = virtio_pci_setup_queue(&g_vd, 0, VI_QSIZE, g_desc, &g_avail,
                                     &g_used, &g_notify);
    if (!g_qsize) { kprintf("virtio-iommu: no request queue\n"); return false; }
    virtio_pci_driver_ok(&g_vd);
    g_last = g_used.idx;
    g_up = true;

    uint64_t in_end = (uint64_t)vp_r32(g_vd.devcfg, VIC_INPUT_END) |
                      ((uint64_t)vp_r32(g_vd.devcfg, VIC_INPUT_END + 4) << 32);
    if (!in_end) in_end = ~0ULL;

    /* ATTACH FIRST, THEN MAP. A domain does not exist until something is in
     * it, so there is no way to install a translation before the endpoint
     * that needs it is attached. This is safe only because nothing has begun
     * doing DMA yet -- which is why this runs immediately after the PCI scan
     * and before any storage or network driver. */
    uint32_t n = pci_devices_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d) continue;
        /* NOT THE IOMMU ITSELF. Its own requests are not translated, and
         * attaching it to a domain is asking it to police itself. */
        if (d->bus == pci->bus && d->device == pci->device &&
            d->function == pci->function) continue;
        /* An endpoint is named by its PCI requester id. */
        uint32_t rid = ((uint32_t)d->bus << 8) |
                       ((uint32_t)d->device << 3) | d->function;
        if (vi_attach(rid)) g_attached++;
    }

    if (g_attached) {
        /* THE FIRST FOUR GIGABYTES, IDENTITY. Everything this kernel hands to
         * a device lives in its own image or in low physical memory, and
         * several of the controllers here cannot express an address wider
         * than 32 bits anyway. */
        uint64_t end = in_end < 0xFFFFFFFFULL ? in_end : 0xFFFFFFFFULL;
        if (!vi_map(0, end)) {
            kprintf("virtio-iommu: the identity map was REFUSED -- every "
                    "attached device can now reach nothing\n");
            return false;
        }
        g_mapped_bytes = end + 1;
    }

    kprintf("virtio-iommu: %u endpoint(s) attached to domain %u, %llu MiB "
            "mapped\n", g_attached, VI_DOMAIN,
            (unsigned long long)(g_mapped_bytes >> 20));
    return true;
}

bool     virtio_iommu_present(void) { return g_up; }
uint32_t virtio_iommu_endpoints(void) { return g_attached; }
uint64_t virtio_iommu_mapped(void) { return g_mapped_bytes; }
