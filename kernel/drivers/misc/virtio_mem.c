/* kernel/drivers/misc/virtio_mem.c -- memory added a block at a time.
 *
 * A pc-dimm is a whole stick: the host adds it, ACPI reports it, and the
 * guest gains all of it at once. virtio-mem is finer-grained. The host
 * reserves a REGION once, at start-up, and then moves a number up and down --
 * "I would like you to be using this much of it" -- and the guest plugs or
 * unplugs blocks inside that region to match.
 *
 * WHICH MAKES THE DEVICE'S REQUEST A REQUEST AND NOT A COMMAND, and that is
 * the part a driver gets wrong. The guest decides how much it can give back,
 * and a guest that cannot reduce its usage right now is entitled to stay
 * where it is. Nothing here unplugs, for exactly that reason: this kernel's
 * page allocator has no way to vacate a range that is already in use, and
 * handing back memory somebody is using is not a slow operation, it is a
 * wrong one. Growing is implemented; shrinking is recorded as not done.
 *
 * A BLOCK IS NOT A PAGE. The device's block size is its own -- typically two
 * megabytes -- and every address handed to it must be a multiple of it. The
 * page allocator learns about the memory afterwards, in its own units.
 *
 * NOT MEASURED, AND SAID SO: this build of QEMU has no virtio-mem device at
 * all ("'virtio-mem-pci' is not a valid device model name"), so nothing past
 * the probe has run. What IS verified is the probe declining -- a machine
 * without the device boots unchanged. Same standing as kernel/drivers/tpm,
 * and recorded the same way in docs/HARDWARE_GAPS.md rather than implied.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/misc/virtio_mem.h"
#include "mm/pmm.h"

#define VIRTIO_MEM_DEVID 0x1058   /* 0x1040 + device type 24 */

/* Configuration space. */
#define VM_BLOCK_SIZE   0
#define VM_NODE_ID      8
#define VM_ADDR         16
#define VM_REGION_SIZE  24
#define VM_USABLE_SIZE  32
#define VM_PLUGGED_SIZE 40
#define VM_REQUESTED    48

#define VM_REQ_PLUG       0
#define VM_REQ_UNPLUG     1
#define VM_REQ_UNPLUG_ALL 2
#define VM_REQ_STATE      3

#define VM_RESP_ACK  0
#define VM_RESP_NACK 1
#define VM_RESP_BUSY 2

#define VM_QSIZE 8

struct vm_req {
    uint16_t type;
    uint16_t padding[3];
    uint64_t addr;
    uint16_t nb_blocks;
    uint16_t padding2[3];
} __attribute__((packed));

struct vm_resp {
    uint16_t type;
    uint16_t padding;
    uint16_t state;
} __attribute__((packed));

static struct virtio_pci_dev g_vd;
static bool g_up;
static uint64_t g_block, g_base, g_region, g_plugged;

static struct vring_desc g_desc[VM_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[VM_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_avail;
static struct { uint16_t flags, idx; struct vring_used_elem ring[VM_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_used;
static struct vm_req  g_req  __attribute__((aligned(64)));
static struct vm_resp g_resp __attribute__((aligned(64)));
static uint16_t g_notify, g_qsize, g_last;

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static uint64_t cfg64(uint32_t off) {
    return (uint64_t)vp_r32(g_vd.devcfg, off) |
           ((uint64_t)vp_r32(g_vd.devcfg, off + 4) << 32);
}

static int vm_request(uint16_t type, uint64_t addr, uint16_t blocks) {
    memset(&g_req, 0, sizeof g_req);
    g_req.type = type;
    g_req.addr = addr;
    g_req.nb_blocks = blocks;
    g_resp.type = 0xFFFF;

    g_desc[0].addr = dma(&g_req);  g_desc[0].len = sizeof g_req;
    g_desc[0].flags = VRING_DESC_F_NEXT; g_desc[0].next = 1;
    g_desc[1].addr = dma(&g_resp); g_desc[1].len = sizeof g_resp;
    g_desc[1].flags = VRING_DESC_F_WRITE; g_desc[1].next = 0;

    g_avail.ring[g_avail.idx % g_qsize] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify, 0);

    for (int spin = 0; spin < 20000000; spin++) {
        if (g_used.idx != g_last) { g_last++; return g_resp.type; }
        __asm__ volatile("" ::: "memory");
    }
    kprintf("virtio-mem: a request was never answered\n");
    return -1;
}

/* Bring the guest's usage up to what the host is asking for. Called at boot
 * and again whenever the poller notices the request has changed. */
uint64_t virtio_mem_sync(void) {
    if (!g_up) return 0;
    uint64_t want = cfg64(VM_REQUESTED);
    uint64_t usable = cfg64(VM_USABLE_SIZE);
    if (want > usable) want = usable;
    if (want <= g_plugged) return 0;       /* nothing to grow into */

    uint64_t added = 0;
    while (g_plugged < want) {
        /* ONE BLOCK AT A TIME, at the next unplugged address. Asking for a
         * run and then failing halfway leaves the driver unsure how much was
         * actually plugged; asking for one leaves no ambiguity. */
        uint64_t addr = g_base + g_plugged;
        int rc = vm_request(VM_REQ_PLUG, addr, 1);
        if (rc == VM_RESP_BUSY) break;     /* the host is not ready; try later */
        if (rc != VM_RESP_ACK) {
            kprintf("virtio-mem: the host refused a block at %llx (%d)\n",
                    (unsigned long long)addr, rc);
            break;
        }
        g_plugged += g_block;
        /* AND ONLY THEN TELL THE ALLOCATOR. Memory that is not plugged is not
         * there: handing it to the page allocator first would let something
         * allocate an address the host has not backed. */
        if (pmm_add_region(addr, g_block)) added += g_block;
    }
    if (added)
        kprintf("virtio-mem: plugged %llu MiB (%llu MiB of %llu MiB region in "
                "use)\n", (unsigned long long)(added >> 20),
                (unsigned long long)(g_plugged >> 20),
                (unsigned long long)(g_region >> 20));
    return added;
}

bool virtio_mem_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_MEM_DEVID, 0);
    if (!pci) return false;
    if (!virtio_pci_attach(&g_vd, pci, "virtio-mem", 0, 0)) return false;
    if (!g_vd.devcfg) { kprintf("virtio-mem: no config window\n"); return false; }

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)&g_avail, 0, sizeof g_avail);
    memset((void *)&g_used, 0, sizeof g_used);
    g_avail.flags = VRING_AVAIL_F_NO_INTERRUPT;
    g_qsize = virtio_pci_setup_queue(&g_vd, 0, VM_QSIZE, g_desc, &g_avail,
                                     &g_used, &g_notify);
    if (!g_qsize) { kprintf("virtio-mem: no request queue\n"); return false; }
    virtio_pci_driver_ok(&g_vd);
    g_last = g_used.idx;

    g_block  = cfg64(VM_BLOCK_SIZE);
    g_base   = cfg64(VM_ADDR);
    g_region = cfg64(VM_REGION_SIZE);
    if (!g_block || !g_region) {
        kprintf("virtio-mem: the device describes no usable region\n");
        return false;
    }
    g_up = true;

    /* START FROM NOTHING PLUGGED, whatever the device thinks. A previous
     * operating system on this machine may have left blocks in, and this
     * kernel has no record of which -- unplugging everything first makes the
     * driver's idea of what is plugged true rather than assumed. */
    vm_request(VM_REQ_UNPLUG_ALL, 0, 0);
    g_plugged = 0;

    kprintf("virtio-mem: %llu MiB region at %llx, %llu KiB blocks\n",
            (unsigned long long)(g_region >> 20), (unsigned long long)g_base,
            (unsigned long long)(g_block >> 10));
    virtio_mem_sync();
    return true;
}

bool     virtio_mem_present(void) { return g_up; }
uint64_t virtio_mem_plugged(void) { return g_plugged; }
uint64_t virtio_mem_region(void)  { return g_region; }
