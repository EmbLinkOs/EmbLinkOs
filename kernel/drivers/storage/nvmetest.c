/* kernel/drivers/storage/nvmetest.c -- the NVMe witness.
 *
 * Shared between the x86 console (`test nvme`) and the aarch64 boot test, like
 * the swap and PIE witnesses: one set of claims, measured on both machines.
 *
 * WHAT IT WILL NOT DO is write to a disk that holds anything. It looks for a
 * namespace whose first block begins with NVME_SCRATCH_MAGIC -- the harness
 * makes such an image (`make build/nvme-scratch.img`) -- and refuses every
 * other one. A self-test that can destroy the user's data by being typed at a
 * console is not a self-test.
 *
 * WHAT IT PROVES, and why each case is here: NVMe describes a transfer's memory
 * as physical pages (PRPs) in three different shapes, and a driver that gets
 * one shape wrong passes every test that only exercises the other two.
 *
 *   one page          PRP1 alone
 *   two pages         PRP2 IS the second page
 *   three or more     PRP2 points at a LIST of the rest
 *   past MDTS         the driver must split it into several commands
 *
 * Every block written carries its own LBA and a per-run seed in every word, so
 * the checks catch the failures that matter and a "read back what I wrote"
 * test would not: pages swapped inside one transfer, an LBA the device ignored,
 * and data left over from a previous run of this very test. */
#include "drivers/storage/nvme.h"
#include "block/block.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "drivers/timer/timer.h"      /* time_get_ns: an [info] rate, not a claim */

#define NVME_SCRATCH_MAGIC "EMBKNVMETEST"

static uint32_t g_seed;

/* Fill `n` blocks starting at `lba` with a pattern that names the block and the
 * run: word i of block b is (lba+b) ^ seed ^ i*2654435761. */
static void fill(uint8_t *buf, uint64_t lba, uint32_t n, uint32_t bs)
{
    for (uint32_t b = 0; b < n; b++) {
        uint32_t *w = (uint32_t *)(buf + (size_t)b * bs);
        for (uint32_t i = 0; i < bs / 4; i++)
            w[i] = (uint32_t)(lba + b) ^ g_seed ^ (i * 2654435761u);
    }
}

/* Returns the index of the first mismatching block, or -1 if all match. */
static long check(const uint8_t *buf, uint64_t lba, uint32_t n, uint32_t bs)
{
    for (uint32_t b = 0; b < n; b++) {
        const uint32_t *w = (const uint32_t *)(buf + (size_t)b * bs);
        for (uint32_t i = 0; i < bs / 4; i++)
            if (w[i] != ((uint32_t)(lba + b) ^ g_seed ^ (i * 2654435761u)))
                return (long)b;
    }
    return -1;
}

/* One write-then-read round trip of `n` blocks at `lba`. */
static int round_trip(struct embk_block_device *d, const char *what,
                      uint64_t lba, uint32_t n, uint8_t *wbuf, uint8_t *rbuf)
{
    uint32_t bs = d->block_size;
    fill(wbuf, lba, n, bs);
    memset(rbuf, 0xA5, (size_t)n * bs);          /* so "not overwritten" shows */
    if (embk_block_write(d, lba, n, wbuf) != EMBK_OK) {
        kprintf("  [FAIL] %s: write of %u block(s) at %llu failed\n", what,
                (unsigned)n, (unsigned long long)lba);
        return -1;
    }
    if (embk_block_read(d, lba, n, rbuf) != EMBK_OK) {
        kprintf("  [FAIL] %s: read of %u block(s) at %llu failed\n", what,
                (unsigned)n, (unsigned long long)lba);
        return -1;
    }
    long bad = check(rbuf, lba, n, bs);
    if (bad >= 0) {
        kprintf("  [FAIL] %s: block %ld of %u came back wrong (lba %llu)\n", what,
                bad, (unsigned)n, (unsigned long long)(lba + (uint64_t)bad));
        return -1;
    }
    kprintf("  [ ok ] %s: %u block(s), %u KiB, written and read back exactly\n",
            what, (unsigned)n, (unsigned)(((uint64_t)n * bs) >> 10));
    return 0;
}

int nvme_selftest_run(void)
{
    int nns = nvme_namespace_count();
    if (nns == 0) {
        kprintf("  [ -- ] no NVMe namespace on this machine\n");
        return -EMBK_ENOENT;
    }

    /* Find the scratch namespace. Every namespace is READ -- that much is safe
     * and itself a check -- but only the marked one is ever written. */
    struct embk_block_device *d = 0;
    int di = -1;
    uint8_t *probe = kmalloc(4096);
    if (!probe) return -EMBK_ENOMEM;
    for (int i = 0; i < nns; i++) {
        struct embk_block_device *c = nvme_namespace_blkdev(i);
        if (!c) continue;
        if (embk_block_read(c, 0, 1, probe) != EMBK_OK) {
            kprintf("  [FAIL] %s: block 0 could not be read\n", c->name);
            kfree(probe);
            return -EMBK_EIO;
        }
        kprintf("  [ ok ] %s: block 0 read (%u-byte blocks)%s\n", c->name,
                (unsigned)c->block_size,
                memcmp(probe, NVME_SCRATCH_MAGIC, sizeof NVME_SCRATCH_MAGIC - 1) == 0
                    ? " -- the scratch disk" : " -- holds data, will NOT be written");
        if (!d && memcmp(probe, NVME_SCRATCH_MAGIC, sizeof NVME_SCRATCH_MAGIC - 1) == 0) {
            d = c;
            di = i;
        }
    }
    kfree(probe);
    if (!d) {
        kprintf("  [ -- ] no namespace carries the scratch marker; nothing written.\n");
        kprintf("         (attach build/nvme-scratch.img to run the write tests)\n");
        return -EMBK_ENOENT;
    }

    uint32_t bs  = d->block_size;
    uint32_t mx  = nvme_namespace_max_xfer(di);
    /* The largest transfer: comfortably more than one command's worth, so the
     * split path runs, whatever MDTS said. */
    uint32_t big = (mx * 3) / bs;
    if (big < 64) big = 64;
    size_t bytes = (size_t)big * bs;
    uint8_t *wbuf = kmalloc(bytes);
    uint8_t *rbuf = kmalloc(bytes);
    if (!wbuf || !rbuf) {
        kfree(wbuf); kfree(rbuf);
        kprintf("  [FAIL] could not allocate %u KiB of test buffers\n", (unsigned)(bytes >> 10));
        return -EMBK_ENOMEM;
    }

    g_seed = (uint32_t)(time_get_ns() ^ 0x9E3779B9u);
    int fails = 0;
    uint32_t per_page = 4096 / bs;                 /* blocks per 4 KiB page */
    uint64_t last = d->block_count;

    /* Block 0 holds the marker and is never written; everything below starts
     * at 8 so that also holds on a 512-byte namespace. */
    fails += round_trip(d, "one page      (PRP1)",           8,                 per_page,     wbuf, rbuf) != 0;
    fails += round_trip(d, "two pages     (PRP2 = page)",    64,                per_page * 2, wbuf, rbuf) != 0;
    fails += round_trip(d, "many pages    (PRP2 = list)",    256,               mx / bs,      wbuf, rbuf) != 0;
    fails += round_trip(d, "past one command (split)",       1024,              big,          wbuf, rbuf) != 0;
    fails += round_trip(d, "the namespace's last blocks",    last - per_page*2, per_page * 2, wbuf, rbuf) != 0;
    if (bs == 512)
        fails += round_trip(d, "one 512-byte block, unaligned", 4097,           1,            wbuf, rbuf) != 0;

    /* The LBA must MATTER: re-read the first region and require the first
     * pattern, not whatever was written last. A driver that dropped CDW10/11
     * would have written everything to block 0 and pass the checks above. */
    if (embk_block_read(d, 8, per_page, rbuf) == EMBK_OK && check(rbuf, 8, per_page, bs) < 0)
        kprintf("  [ ok ] earlier writes are still where they were written -- the LBA is honoured\n");
    else {
        kprintf("  [FAIL] re-reading block 8 did not return what was written there\n");
        fails++;
    }

    /* Out of range is refused by the block layer before it reaches the device. */
    if (embk_block_read(d, last, 1, rbuf) != EMBK_OK)
        kprintf("  [ ok ] a read past the end of the namespace is refused\n");
    else {
        kprintf("  [FAIL] a read past the end of the namespace was allowed\n");
        fails++;
    }

    if (embk_block_flush(d) == EMBK_OK)
        kprintf("  [ ok ] flush completed\n");
    else {
        kprintf("  [FAIL] flush failed\n");
        fails++;
    }

    /* The power-off that does not happen. power_transition() notifies every
     * drive and, when the firmware fails to take the machine -- real hardware
     * with no ACPI interpreter, today -- restarts them. That restart cannot be
     * seen on an emulator whose power-off always works, so run it here: after
     * a shutdown notification and a restart, the data written above must
     * still be there and a new round trip must work. */
    if (nvme_cycle_namespace_controller(di) != 0) {
        kprintf("  [FAIL] the controller did not come back after a shutdown notification\n");
        fails++;
    } else if (embk_block_read(d, 256, mx / bs, rbuf) != EMBK_OK || check(rbuf, 256, mx / bs, bs) >= 0) {
        kprintf("  [FAIL] after shutdown + restart, earlier data did not read back\n");
        fails++;
    } else {
        kprintf("  [ ok ] shutdown notification acknowledged, controller restarted, data intact\n");
        fails += round_trip(d, "after the restart", 2048, per_page * 2, wbuf, rbuf) != 0;
    }

    /* [info] only: an emulated device's rate says nothing about a real SSD's. */
    uint64_t t0 = time_get_ns();
    int reps = 8;
    for (int r = 0; r < reps; r++)
        (void)embk_block_read(d, 1024, big, rbuf);
    uint64_t dt = time_get_ns() - t0;
    if (dt > 0)
        kprintf("  [info] sequential read: %llu KiB/s through the bounce buffer (emulated -- not a claim)\n",
                (unsigned long long)(((uint64_t)reps * bytes * 1000000000ull / dt) >> 10));

    kfree(wbuf);
    kfree(rbuf);
    return fails ? -EMBK_EIO : 0;
}
