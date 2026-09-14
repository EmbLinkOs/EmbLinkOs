/* user/tools/install/install.c -- put this operating system onto a disk.
 *
 * docs/PILLARS.md phase 2: "There is no way to put the OS onto the target's
 * disk". You could build a USB stick on a development machine and boot from
 * it, and that was all. An operating system you cannot install is a
 * demonstration.
 *
 * WHAT THIS DOES, and why it is a copy rather than a build. The medium this is
 * running from ALREADY has the right layout: a GPT with an EFI system
 * partition holding the loader and an EMBKFS partition holding the system.
 * Reproducing that on the target means writing an equivalent partition table
 * and copying the partitions across -- no mkfs, no second implementation of
 * EMBKFS's on-disk format to drift from the first one, and the thing installed
 * is byte-for-byte the thing that was tested.
 *
 * The cost is honest and worth stating: the target's root partition comes out
 * the same SIZE as the source's, so installing from a 200 MB stick onto a 1 TB
 * disk uses 200 MB of it. Growing the filesystem afterwards is a separate
 * operation and it is not written yet (docs/TODO.md).
 *
 * WHAT IT REFUSES TO DO. It will not write to the disk it is running from, and
 * it will not write to a partition. Both are cheap to check and both are
 * unrecoverable if you get them wrong, which is the definition of a check
 * worth making.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "embk.h"

#define SECTOR 512
#define CHUNK  EMBK_DISK_MAX_BLOCKS          /* blocks per syscall */

/* ---- GPT ---------------------------------------------------------------- */
struct gpt_header {
    char     sig[8];              /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba, alt_lba, first_usable, last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size;
    uint32_t entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba, last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed));

static uint32_t crc32(const void *data, size_t len) {
    static uint32_t tab[256];
    static int built;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        built = 1;
    }
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- disks -------------------------------------------------------------- */
static int find_disk(const char *name, struct embk_disk_info *out) {
    int n = embk_disk_count();
    for (int i = 0; i < n; i++) {
        struct embk_disk_info d;
        if (embk_disk_info(i, &d) != 0) continue;
        if (strcmp(d.name, name) == 0) { *out = d; return i; }
    }
    return -1;
}

static int copy_blocks(int src, uint64_t src_lba, int dst, uint64_t dst_lba,
                       uint64_t blocks, const char *what) {
    static uint8_t buf[CHUNK * SECTOR];
    uint64_t done = 0;
    uint64_t last_pct = 101;
    while (done < blocks) {
        uint32_t n = (uint32_t)((blocks - done) < CHUNK ? (blocks - done) : CHUNK);
        if (embk_disk_read(src, src_lba + done, n, buf) != 0) {
            printf("install: read failed at LBA %llu of %s\n",
                   (unsigned long long)(src_lba + done), what);
            return -1;
        }
        if (embk_disk_write(dst, dst_lba + done, n, buf) != 0) {
            printf("install: write failed at LBA %llu of %s\n",
                   (unsigned long long)(dst_lba + done), what);
            return -1;
        }
        done += n;
        uint64_t pct = blocks ? (done * 100 / blocks) : 100;
        if (pct != last_pct && (pct % 10) == 0) {
            printf("install:   %s %llu%%\n", what, (unsigned long long)pct);
            last_pct = pct;
        }
    }
    return 0;
}

static void list_disks(void) {
    int n = embk_disk_count();
    printf("install: %d block device(s):\n", n);
    for (int i = 0; i < n; i++) {
        struct embk_disk_info d;
        if (embk_disk_info(i, &d) != 0) continue;
        printf("  [%d] %-6s %8llu MB  %s\n", i, d.name,
               (unsigned long long)((d.block_count * d.block_size) >> 20),
               (d.flags & EMBK_DISK_IS_PARTITION) ? "partition" : "whole disk");
    }
}

int main(int argc, char **argv) {
    printf("EmbLinkOS installer\n");

    if (argc < 3) {
        list_disks();
        printf("\nusage: install <source disk> <target disk>\n"
               "  e.g. install sda sdb\n"
               "  Both must be WHOLE DISKS. Everything on the target is lost.\n");
        return argc < 2 ? 0 : 2;
    }

    struct embk_disk_info src, dst;
    int si = find_disk(argv[1], &src);
    int di = find_disk(argv[2], &dst);
    if (si < 0) { printf("install: no such disk: %s\n", argv[1]); return 2; }
    if (di < 0) { printf("install: no such disk: %s\n", argv[2]); return 2; }

    /* THE TWO REFUSALS. Writing a partition table into a partition produces a
     * disk that looks installed and boots nothing; installing onto the source
     * destroys the thing being copied halfway through. */
    if (src.flags & EMBK_DISK_IS_PARTITION) {
        printf("install: %s is a partition, not a disk\n", argv[1]);
        return 2;
    }
    if (dst.flags & EMBK_DISK_IS_PARTITION) {
        printf("install: %s is a partition, not a disk -- give me the whole "
               "disk to partition\n", argv[2]);
        return 2;
    }
    if (si == di) {
        printf("install: source and target are the same disk\n");
        return 2;
    }

    printf("install: source %s (%llu MB), target %s (%llu MB)\n",
           src.name, (unsigned long long)((src.block_count * src.block_size) >> 20),
           dst.name, (unsigned long long)((dst.block_count * dst.block_size) >> 20));

    /* ---- read the source's partition table --------------------------- */
    static uint8_t sec[SECTOR];
    if (embk_disk_read(si, 1, 1, sec) != 0) {
        printf("install: cannot read %s's GPT header\n", src.name);
        return 1;
    }
    struct gpt_header gh;
    memcpy(&gh, sec, sizeof gh);
    if (memcmp(gh.sig, "EFI PART", 8) != 0) {
        printf("install: %s has no GPT -- this installer copies a GPT layout "
               "and cannot create one from nothing\n", src.name);
        return 1;
    }

    uint32_t nent = gh.entry_count;
    uint32_t esz  = gh.entry_size;
    if (nent == 0 || nent > 128 || esz < sizeof(struct gpt_entry) || esz > 512) {
        printf("install: %s's GPT is shaped oddly (%u entries of %u bytes)\n",
               src.name, nent, esz);
        return 1;
    }

    uint64_t table_bytes = (uint64_t)nent * esz;
    uint64_t table_blocks = (table_bytes + SECTOR - 1) / SECTOR;
    static uint8_t table[128 * 128];
    if (table_bytes > sizeof table) { printf("install: GPT table too large\n"); return 1; }
    for (uint64_t b = 0; b < table_blocks; b += CHUNK) {
        uint32_t n = (uint32_t)((table_blocks - b) < CHUNK ? (table_blocks - b) : CHUNK);
        if (embk_disk_read(si, gh.entries_lba + b, n, table + b * SECTOR) != 0) {
            printf("install: cannot read %s's partition entries\n", src.name);
            return 1;
        }
    }

    /* How far the source's partitions actually reach. Everything up to there
     * has to fit on the target. */
    uint64_t highest = 0;
    int nparts = 0;
    for (uint32_t i = 0; i < nent; i++) {
        struct gpt_entry e;
        memcpy(&e, table + (uint64_t)i * esz, sizeof e);
        int empty = 1;
        for (int k = 0; k < 16; k++) if (e.type_guid[k]) empty = 0;
        if (empty) continue;
        nparts++;
        if (e.last_lba > highest) highest = e.last_lba;
        printf("install:   partition %d: LBA %llu..%llu (%llu MB)\n", nparts,
               (unsigned long long)e.first_lba, (unsigned long long)e.last_lba,
               (unsigned long long)(((e.last_lba - e.first_lba + 1) * SECTOR) >> 20));
    }
    if (nparts == 0) { printf("install: %s has no partitions\n", src.name); return 1; }

    /* The target needs room for everything up to `highest`, PLUS its own
     * backup GPT at the very end -- which is at a different LBA than the
     * source's, because the disks are different sizes. */
    if (dst.block_count < highest + table_blocks + 2) {
        printf("install: %s is too small (needs %llu blocks, has %llu)\n",
               dst.name, (unsigned long long)(highest + table_blocks + 2),
               (unsigned long long)dst.block_count);
        return 1;
    }

    /* ---- copy everything from the start through the last partition ----
     *
     * INCLUDING THE PROTECTIVE MBR AND THE PRIMARY GPT, because they are
     * already correct for these partition positions -- the partitions land at
     * the same LBAs on the target. Only the BACKUP header at the end of the
     * disk has to be rebuilt, and only because "the end of the disk" is a
     * different place. */
    printf("install: copying %llu MB\n",
           (unsigned long long)(((highest + 1) * SECTOR) >> 20));
    if (copy_blocks(si, 0, di, 0, highest + 1, "image") != 0) return 1;

    /* ---- rebuild the target's backup GPT ------------------------------ */
    uint64_t alt_lba = dst.block_count - 1;
    uint64_t alt_entries = alt_lba - table_blocks;

    /* The primary header on the target still says the backup lives where the
     * SOURCE's did. Fix both copies: a GPT whose two headers disagree is one
     * that firmware may "repair" by restoring the wrong one. */
    gh.alt_lba      = alt_lba;
    gh.last_usable  = alt_entries - 1;
    gh.header_crc   = 0;
    gh.header_crc   = crc32(&gh, gh.header_size);
    memset(sec, 0, SECTOR);
    memcpy(sec, &gh, sizeof gh);
    if (embk_disk_write(di, 1, 1, sec) != 0) {
        printf("install: cannot rewrite the primary GPT header\n");
        return 1;
    }

    struct gpt_header bh = gh;
    bh.my_lba      = alt_lba;
    bh.alt_lba     = 1;
    bh.entries_lba = alt_entries;
    bh.header_crc  = 0;
    bh.header_crc  = crc32(&bh, bh.header_size);

    for (uint64_t b = 0; b < table_blocks; b += CHUNK) {
        uint32_t n = (uint32_t)((table_blocks - b) < CHUNK ? (table_blocks - b) : CHUNK);
        if (embk_disk_write(di, alt_entries + b, n, table + b * SECTOR) != 0) {
            printf("install: cannot write the backup partition entries\n");
            return 1;
        }
    }
    memset(sec, 0, SECTOR);
    memcpy(sec, &bh, sizeof bh);
    if (embk_disk_write(di, alt_lba, 1, sec) != 0) {
        printf("install: cannot write the backup GPT header\n");
        return 1;
    }

    printf("install: DONE -- %s now carries EmbLinkOS\n", dst.name);
    printf("install: the firmware will find it at /EFI/BOOT/BOOTX64.EFI, which\n"
           "install: is the removable-media path every UEFI machine tries\n"
           "install: without needing a boot entry registered first.\n");
    return 0;
}
