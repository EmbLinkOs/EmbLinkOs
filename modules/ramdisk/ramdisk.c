/* modules/ramdisk/ramdisk.c -- a real driver, loaded after the kernel booted.
 *
 * A RAM disk is the right first module because it is a REAL driver and not a
 * demonstration: it registers a block device, serves reads and writes, and
 * appears in the block table beside the SATA and NVMe disks. Everything that
 * has to work for a wireless driver to be a module has to work for this one --
 * calling into the kernel, keeping state, registering with a subsystem, and
 * taking itself back out again on unload.
 *
 * IT ALSO EXERCISES EVERY RELOCATION KIND that matters. The string constants
 * are absolute references into the module's own .rodata; the calls to kprintf
 * and kmalloc are 32-bit PC-relative into the kernel; the block-device struct
 * holds function pointers, which are 64-bit absolute. A loader that gets any
 * of those wrong fails here rather than in somebody's wireless driver.
 */
#include "module/module.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/kmalloc.h"
#include "include/errno.h"
#include "block/block.h"

#define RD_BLOCK_SIZE 512
#define RD_BLOCKS     2048            /* 1 MiB, small enough to be polite */

static uint8_t *g_store;
static struct embk_block_device g_dev;

static int rd_read(struct embk_block_device *dev, uint64_t lba,
                   uint32_t count, void *buffer) {
    (void)dev;
    if (lba + count > RD_BLOCKS) return -EMBK_EINVAL;
    memcpy(buffer, g_store + lba * RD_BLOCK_SIZE, count * RD_BLOCK_SIZE);
    return EMBK_OK;
}

static int rd_write(struct embk_block_device *dev, uint64_t lba,
                    uint32_t count, const void *buffer) {
    (void)dev;
    if (lba + count > RD_BLOCKS) return -EMBK_EINVAL;
    memcpy(g_store + lba * RD_BLOCK_SIZE, buffer, count * RD_BLOCK_SIZE);
    return EMBK_OK;
}

static int rd_init(void) {
    g_store = kmalloc((uint64_t)RD_BLOCKS * RD_BLOCK_SIZE);
    if (!g_store) {
        kprintf("ramdisk: no memory for %u blocks\n", (unsigned)RD_BLOCKS);
        return -EMBK_ENOMEM;
    }
    memset(g_store, 0, (uint64_t)RD_BLOCKS * RD_BLOCK_SIZE);

    memset(&g_dev, 0, sizeof g_dev);
    g_dev.block_count = RD_BLOCKS;
    g_dev.block_size  = RD_BLOCK_SIZE;
    g_dev.read  = rd_read;
    g_dev.write = rd_write;
    g_dev.flush = NULL;             /* it IS the cache */
    g_dev.dma_max_phys = ~0ULL;
    g_dev.needs_kernel_range = true;

    int rc = embk_block_register(&g_dev);
    if (rc != EMBK_OK) {
        kprintf("ramdisk: the block layer refused it (%d)\n", rc);
        kfree(g_store);
        g_store = 0;
        return rc;
    }

    /* A MARKER THE TEST CAN READ BACK. Writing through our own op and reading
     * it back through the block layer's is what separates "registered" from
     * "works" -- the same distinction the USB hot-plug test draws between a
     * mounted volume and a readable one. */
    static const char marker[] = "EMBLINK-RAMDISK-MODULE";
    memcpy(g_store, marker, sizeof marker);

    kprintf("ramdisk: %s registered, %u blocks x %u bytes\n",
            g_dev.name, (unsigned)RD_BLOCKS, (unsigned)RD_BLOCK_SIZE);
    return 0;
}

static void rd_exit(void) {
    embk_block_unregister(&g_dev);
    kfree(g_store);
    g_store = 0;
    kprintf("ramdisk: gone\n");
}

EMBK_MODULE("ramdisk", "EmbLinkOS", "a block device that lives in memory",
            rd_init, rd_exit);
