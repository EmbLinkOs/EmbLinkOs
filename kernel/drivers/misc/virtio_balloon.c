/* kernel/drivers/misc/virtio_balloon.c -- giving memory back to the host.
 *
 * A virtual machine is given its memory once and holds it whether or not it is
 * using it. The balloon is how the host asks for some back: it names a target
 * size, the guest hands over PAGES IT PROMISES NOT TO TOUCH, and the host can
 * then use them elsewhere. Deflating is the reverse.
 *
 * THE PROMISE IS THE WHOLE MECHANISM AND NOTHING ENFORCES IT. A page handed to
 * the host and then written by the guest reads back as whatever the host put
 * there -- no fault, no error, just corruption. So the pages come from the
 * physical allocator and are held by the balloon for as long as they are
 * inflated; they are never "borrowed" from something in use.
 *
 * The guest is allowed to refuse. A host asking for more than can be spared
 * gets what there is, which is the honest answer and what the specification
 * expects -- `actual` is a report, not an acknowledgement.
 */
#include <stdint.h>
#include <stddef.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/misc/virtio_balloon.h"
#include "mm/pmm.h"

#define VIRTIO_BALLOON_DEVID_M 0x1045   /* 0x1040 + device type 5 */
#define VIRTIO_BALLOON_DEVID_T 0x1002

#define VB_QSIZE 8
#define VB_BATCH 256                    /* page frame numbers per request */
#define VB_MAX_PAGES 4096               /* the most this will ever hold: 16 MiB */

static struct virtio_pci_dev g_vd;
static bool g_up;

/* Two queues, two rings. Inflate and deflate are separate queues and a single
 * shared ring would make a deflate wait behind an inflate that the host is
 * still working through. */
static struct vring_desc g_desc[2][VB_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[VB_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_avail[2];
static struct { uint16_t flags, idx; struct vring_used_elem ring[VB_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_used[2];
static uint32_t g_pfns[VB_BATCH] __attribute__((aligned(64)));
static uint16_t g_notify[2], g_qsize[2], g_last[2];

/* The frames we are holding. Kept so deflating gives back the SAME frames --
 * handing the host a frame we never took is a page it will hand to somebody
 * else while the guest still owns it. */
static uint64_t g_held[VB_MAX_PAGES];
static uint32_t g_held_n;

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

static void vb_submit(int q, uint32_t count) {
    g_desc[q][0].addr = dma(g_pfns);
    g_desc[q][0].len = count * 4;
    g_desc[q][0].flags = 0;             /* the device READS the list */
    g_desc[q][0].next = 0;
    g_avail[q].ring[g_avail[q].idx % g_qsize[q]] = 0;
    __sync_synchronize();
    g_avail[q].idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify[q], (uint16_t)q);
    for (int spin = 0; spin < 5000000; spin++) {
        if (g_used[q].idx != g_last[q]) { g_last[q]++; return; }
        __asm__ volatile("" ::: "memory");
    }
}

/* Tell the host how much we are actually holding. This is `actual` in config
 * space, and it is the only thing the host has to go on -- a guest that
 * inflates and never updates it looks like a guest that ignored the request. */
static void vb_report(void) {
    if (g_vd.devcfg) vp_w32(g_vd.devcfg, 4, g_held_n);
}

uint32_t virtio_balloon_inflate(uint32_t pages) {
    if (!g_up) return 0;
    uint32_t done = 0;
    while (done < pages && g_held_n < VB_MAX_PAGES) {
        uint32_t batch = 0;
        while (batch < VB_BATCH && done + batch < pages && g_held_n < VB_MAX_PAGES) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) break;                 /* the guest keeps what it needs */
            g_held[g_held_n++] = phys;
            /* THE UNIT IS A 4 KiB PAGE FRAME NUMBER, always, whatever the
             * guest's own page size is. Sending an address instead hands the
             * host a frame number a million times too large. */
            g_pfns[batch++] = (uint32_t)(phys >> 12);
        }
        if (!batch) break;
        vb_submit(0, batch);                  /* queue 0: inflate */
        done += batch;
    }
    vb_report();
    return done;
}

uint32_t virtio_balloon_deflate(uint32_t pages) {
    if (!g_up) return 0;
    uint32_t done = 0;
    while (done < pages && g_held_n) {
        uint32_t batch = 0;
        while (batch < VB_BATCH && done + batch < pages && g_held_n) {
            uint64_t phys = g_held[--g_held_n];
            g_pfns[batch++] = (uint32_t)(phys >> 12);
        }
        vb_submit(1, batch);                  /* queue 1: deflate */
        /* Only free AFTER the host has been told: a page given back to the
         * allocator while the host still believes it owns it is the same
         * corruption as never having inflated it. */
        for (uint32_t i = 0; i < batch; i++)
            pmm_free_page((uint64_t)g_pfns[i] << 12);
        done += batch;
    }
    vb_report();
    return done;
}

bool virtio_balloon_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_BALLOON_DEVID_M, 0);
    if (!pci) pci = virtio_pci_find(VIRTIO_BALLOON_DEVID_T, 0);
    if (!pci) return false;
    if (!virtio_pci_attach(&g_vd, pci, "virtio-balloon", 0, 0)) return false;

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)g_avail, 0, sizeof g_avail);
    memset((void *)g_used, 0, sizeof g_used);
    for (int q = 0; q < 2; q++) {
        g_avail[q].flags = VRING_AVAIL_F_NO_INTERRUPT;
        g_qsize[q] = virtio_pci_setup_queue(&g_vd, (uint16_t)q, VB_QSIZE,
                                            g_desc[q], &g_avail[q], &g_used[q],
                                            &g_notify[q]);
        if (!g_qsize[q]) { kprintf("virtio-balloon: queue %d refused\n", q); return false; }
    }
    virtio_pci_driver_ok(&g_vd);
    g_last[0] = g_used[0].idx;
    g_last[1] = g_used[1].idx;
    g_up = true;
    kprintf("virtio-balloon: ready (host wants %u pages)\n", virtio_balloon_target());
    return true;
}

bool virtio_balloon_present(void) { return g_up; }
uint32_t virtio_balloon_held(void) { return g_held_n; }

/* What the host is ASKING for, in 4 KiB pages, from configuration space. The
 * guest decides what to do about it -- nothing here inflates on its own. */
uint32_t virtio_balloon_target(void) {
    if (!g_up || !g_vd.devcfg) return 0;
    return vp_r32(g_vd.devcfg, 0);
}
