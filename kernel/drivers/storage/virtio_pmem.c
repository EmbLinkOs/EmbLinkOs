/* kernel/drivers/storage/virtio_pmem.c -- memory that survives a reboot.
 *
 * Every other storage driver in this tree moves BLOCKS: it hands the hardware
 * an address and a count and waits. This one does not move anything. The
 * device's whole job is to put a region of persistent memory into the guest's
 * PHYSICAL ADDRESS SPACE and tell the driver where it landed; after that,
 * reading and writing it is load and store instructions, and the only thing
 * the virtqueue is for is FLUSHING -- telling the host "make what I have
 * written durable now".
 *
 * THAT IS THE WHOLE DIFFICULTY, AND IT IS A CORRECTNESS ONE. A store that has
 * reached memory has not reached the backing file. Nothing in the processor
 * says otherwise, and a machine that loses power between the two has lost the
 * write. So every write path here ends in a flush request, and the block
 * device's flush op is the same request issued on its own.
 *
 * The region is mapped UNCACHEABLE. That costs real speed -- it is the one
 * thing that would make this fast -- and it is deliberate for now: cached
 * mappings need explicit cache-line writeback before each flush to mean
 * anything, and a persistence guarantee that is nearly right is worse than a
 * slow one that is right. Recorded in docs/TODO.md rather than papered over.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/bus/virtio_pci.h"
#include "drivers/storage/virtio_pmem.h"
#include "block/block.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define VIRTIO_PMEM_DEVID 0x105B    /* 0x1040 + device type 27 */

#define PMEM_QSIZE 8
#define PMEM_REQ_FLUSH 0

static struct virtio_pci_dev g_vd;
static bool g_up;

static struct vring_desc g_desc[PMEM_QSIZE] __attribute__((aligned(16)));
static struct { uint16_t flags, idx; uint16_t ring[PMEM_QSIZE]; uint16_t ue; }
    __attribute__((packed, aligned(2))) g_avail;
static struct { uint16_t flags, idx; struct vring_used_elem ring[PMEM_QSIZE]; uint16_t ae; }
    __attribute__((packed, aligned(4))) g_used;
static uint32_t g_req __attribute__((aligned(64)));
static uint32_t g_resp __attribute__((aligned(64)));
static uint16_t g_notify, g_qsize, g_last;

static volatile uint8_t *g_mem;     /* the persistent region, mapped */
static uint64_t g_phys, g_size;
static struct embk_block_device g_blk;
static uint64_t g_flushes;

static inline uint64_t dma(const volatile void *p) { return KV2P((uint64_t)(uintptr_t)p); }

/* Ask the host to make everything written so far durable. Synchronous,
 * because there is nothing useful a caller can do with "it will be durable
 * eventually" -- that is the state it was already in. */
static int pmem_flush_now(void) {
    if (!g_up) return -EMBK_ENODEV;
    g_req = PMEM_REQ_FLUSH;
    g_resp = 0xFFFFFFFFu;

    g_desc[0].addr = dma(&g_req);  g_desc[0].len = 4;
    g_desc[0].flags = VRING_DESC_F_NEXT; g_desc[0].next = 1;
    g_desc[1].addr = dma(&g_resp); g_desc[1].len = 4;
    g_desc[1].flags = VRING_DESC_F_WRITE; g_desc[1].next = 0;

    g_avail.ring[g_avail.idx % g_qsize] = 0;
    __sync_synchronize();
    g_avail.idx++;
    __sync_synchronize();
    virtio_pci_notify(&g_vd, g_notify, 0);

    for (int spin = 0; spin < 20000000; spin++) {
        if (g_used.idx != g_last) {
            g_last++;
            g_flushes++;
            return g_resp == 0 ? EMBK_OK : -EMBK_EIO;
        }
        __asm__ volatile("" ::: "memory");
    }
    kprintf("virtio-pmem: the host never answered a flush\n");
    return -EMBK_EIO;
}

static int pmem_read(struct embk_block_device *d, uint64_t lba,
                     uint32_t count, void *buf) {
    (void)d;
    uint64_t off = lba * 512ULL, len = (uint64_t)count * 512ULL;
    if (off + len > g_size) return -EMBK_EINVAL;
    memcpy(buf, (const void *)(g_mem + off), (size_t)len);
    return EMBK_OK;
}

static int pmem_write(struct embk_block_device *d, uint64_t lba,
                      uint32_t count, const void *buf) {
    (void)d;
    uint64_t off = lba * 512ULL, len = (uint64_t)count * 512ULL;
    if (off + len > g_size) return -EMBK_EINVAL;
    memcpy((void *)(g_mem + off), buf, (size_t)len);
    /* A STORE IS NOT A COMMIT. Without this the bytes are in the region and
     * the region is not on the disk behind it, and nothing in the machine
     * will ever say so. */
    return pmem_flush_now();
}

static int pmem_flush_op(struct embk_block_device *d) {
    (void)d;
    return pmem_flush_now();
}

bool virtio_pmem_init(void) {
    if (g_up) return true;
    const struct pci_device *pci = virtio_pci_find(VIRTIO_PMEM_DEVID, 0);
    if (!pci) return false;
    if (!virtio_pci_attach(&g_vd, pci, "virtio-pmem", 0, 0)) return false;
    if (!g_vd.devcfg) {
        kprintf("virtio-pmem: no config window, so no way to find the region\n");
        return false;
    }

    /* WHERE THE REGION LANDED IS THE DEVICE'S ANSWER, NOT A BAR. This memory
     * is not behind a PCI window at all -- the host placed it in the guest's
     * physical address space (out of the machine's memory-hotplug range) and
     * these two fields are the only record of where. */
    g_phys = (uint64_t)vp_r32(g_vd.devcfg, 0) |
             ((uint64_t)vp_r32(g_vd.devcfg, 4) << 32);
    g_size = (uint64_t)vp_r32(g_vd.devcfg, 8) |
             ((uint64_t)vp_r32(g_vd.devcfg, 12) << 32);
    if (!g_size) {
        kprintf("virtio-pmem: the device reports a zero-length region\n");
        return false;
    }

    memset(g_desc, 0, sizeof g_desc);
    memset((void *)&g_avail, 0, sizeof g_avail);
    memset((void *)&g_used, 0, sizeof g_used);
    g_avail.flags = VRING_AVAIL_F_NO_INTERRUPT;
    g_qsize = virtio_pci_setup_queue(&g_vd, 0, PMEM_QSIZE, g_desc, &g_avail,
                                     &g_used, &g_notify);
    if (!g_qsize) { kprintf("virtio-pmem: no request queue\n"); return false; }
    virtio_pci_driver_ok(&g_vd);
    g_last = g_used.idx;

    g_mem = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(g_phys, g_size);
    if (!g_mem) {
        kprintf("virtio-pmem: could not map %llu MiB at %llx\n",
                (unsigned long long)(g_size >> 20), (unsigned long long)g_phys);
        return false;
    }
    g_up = true;

    memset(&g_blk, 0, sizeof g_blk);
    g_blk.block_count = g_size / 512;
    g_blk.block_size  = 512;
    g_blk.read  = pmem_read;
    g_blk.write = pmem_write;
    g_blk.flush = pmem_flush_op;
    g_blk.driver_data = NULL;
    g_blk.dma_max_phys = ~0ULL;
    g_blk.needs_kernel_range = false;   /* nothing DMAs: this is memcpy */
    if (embk_block_register(&g_blk) != EMBK_OK) {
        kprintf("virtio-pmem: block registration failed\n");
        return false;
    }

    kprintf("virtio-pmem: %s = %llu MiB of persistent memory at %llx\n",
            g_blk.name, (unsigned long long)(g_size >> 20),
            (unsigned long long)g_phys);
    return true;
}

bool     virtio_pmem_present(void)  { return g_up; }
uint64_t virtio_pmem_size(void)     { return g_size; }
uint64_t virtio_pmem_flushes(void)  { return g_flushes; }
int      virtio_pmem_flush(void)    { return pmem_flush_now(); }
