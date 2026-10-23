/* kernel/drivers/storage/nvdimm.c -- persistent memory that is not a device.
 *
 * virtio_pmem.c drives a paravirtual persistent region: there is a PCI device,
 * it says where its memory is, and a queue exists to flush it. A real NVDIMM
 * has none of that. It is memory in a slot, and the only thing that knows it
 * is different from ordinary RAM is an ACPI table.
 *
 * So the whole driver is: read the NFIT, find the address ranges whose type
 * GUID says "persistent memory", and expose each as a block device. There is
 * no controller to program and nothing to initialise.
 *
 * THE GUID IS THE ONLY THING THAT DISTINGUISHES THEM. The same table
 * describes volatile ranges, control regions and block windows in exactly the
 * same structure, and a driver that takes every address range it finds will
 * cheerfully offer the machine's ordinary memory as a disk.
 *
 * DURABILITY, honestly: the range is mapped uncacheable, so a store reaches
 * the medium rather than sitting in a cache line -- which is correct and
 * slow, the same trade virtio_pmem.c makes and for the same reason. A cached
 * mapping needs explicit cache-line writeback to mean anything, and a
 * persistence guarantee that is nearly right is worse than a slow one that is
 * right.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "acpi/acpi.h"
#include "drivers/storage/nvdimm.h"
#include "block/block.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define NFIT_TYPE_SPA_RANGE 0

/* 66F0D379-B4F3-4074-AC43-0D3318B78CDB, in the mixed-endian form a GUID is
 * stored in: the first three fields little-endian, the last two as written. */
static const uint8_t PERSISTENT_MEMORY_GUID[16] = {
    0x79, 0xD3, 0xF0, 0x66, 0xF3, 0xB4, 0x74, 0x40,
    0xAC, 0x43, 0x0D, 0x33, 0x18, 0xB7, 0x8C, 0xDB
};

#define NVDIMM_MAX 2

struct nv_region {
    bool used;
    volatile uint8_t *mem;
    uint64_t phys, length;
    struct embk_block_device blk;
};
static struct nv_region g_regions[NVDIMM_MAX];
static uint32_t g_count;

static int nv_read(struct embk_block_device *d, uint64_t lba,
                   uint32_t count, void *buf) {
    struct nv_region *r = d->driver_data;
    uint64_t off = lba * 512ULL, len = (uint64_t)count * 512ULL;
    if (off + len > r->length) return -EMBK_EINVAL;
    memcpy(buf, (const void *)(r->mem + off), (size_t)len);
    return EMBK_OK;
}

static int nv_write(struct embk_block_device *d, uint64_t lba,
                    uint32_t count, const void *buf) {
    struct nv_region *r = d->driver_data;
    uint64_t off = lba * 512ULL, len = (uint64_t)count * 512ULL;
    if (off + len > r->length) return -EMBK_EINVAL;
    memcpy((void *)(r->mem + off), buf, (size_t)len);
    return EMBK_OK;
}

static int nv_flush(struct embk_block_device *d) {
    (void)d;
    /* NOTHING TO DO, AND THAT IS A STATEMENT ABOUT THE MAPPING, not a stub.
     * The range is uncacheable, so a store has already reached the medium by
     * the time it retires. The moment this is mapped cacheable for speed,
     * this function has to write back the cache lines and this comment
     * becomes wrong -- which is why it says which fact it depends on. */
    return EMBK_OK;
}

bool nvdimm_init(void) {
    if (g_count) return true;
    const struct acpi_sdt_header *nfit = acpi_find_table("NFIT");
    if (!nfit) return false;

    /* The NFIT header is 40 bytes: the standard ACPI one plus a reserved
     * word, then a sequence of length-prefixed structures. */
    const uint8_t *p = (const uint8_t *)nfit + 40;
    const uint8_t *end = (const uint8_t *)nfit + nfit->length;

    while (p + 4 <= end && g_count < NVDIMM_MAX) {
        uint16_t type = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t len  = (uint16_t)(p[2] | (p[3] << 8));
        if (len < 4 || p + len > end) break;

        if (type == NFIT_TYPE_SPA_RANGE && len >= 56 &&
            memcmp(p + 16, PERSISTENT_MEMORY_GUID, 16) == 0) {
            uint64_t base = 0, length = 0;
            for (int i = 0; i < 8; i++) {
                base   |= (uint64_t)p[32 + i] << (i * 8);
                length |= (uint64_t)p[40 + i] << (i * 8);
            }
            if (length >= 512) {
                struct nv_region *r = &g_regions[g_count];
                memset(r, 0, sizeof *r);
                r->phys = base;
                r->length = length;
                r->mem = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(base, length);
                if (!r->mem) {
                    kprintf("nvdimm: could not map %llu MiB at %llx\n",
                            (unsigned long long)(length >> 20),
                            (unsigned long long)base);
                } else {
                    r->used = true;
                    r->blk.block_count = length / 512;
                    r->blk.block_size = 512;
                    r->blk.read = nv_read;
                    r->blk.write = nv_write;
                    r->blk.flush = nv_flush;
                    r->blk.driver_data = r;
                    r->blk.dma_max_phys = ~0ULL;
                    r->blk.needs_kernel_range = false;   /* memcpy, not DMA */
                    if (embk_block_register(&r->blk) == EMBK_OK) {
                        kprintf("nvdimm: %s = %llu MiB of persistent memory "
                                "at %llx\n", r->blk.name,
                                (unsigned long long)(length >> 20),
                                (unsigned long long)base);
                        g_count++;
                    } else {
                        r->used = false;
                    }
                }
            }
        }
        p += len;
    }
    return g_count > 0;
}

uint32_t nvdimm_count(void) { return g_count; }
uint64_t nvdimm_bytes(uint32_t i) {
    return i < g_count ? g_regions[i].length : 0;
}
