#ifndef _EMBK_SWAP_H_
#define _EMBK_SWAP_H_
#include <stdint.h>
#include "include/types.h"

/* ==========================================================================
 * THE SWAP STORE -- where a page goes when it is evicted and has no file.
 *
 * File-backed pages already have a backing store: the file. Reclaiming one is
 * free if it is clean and a writeback if it is dirty. Anonymous memory -- the
 * heap, mmap(MAP_ANONYMOUS), a stack -- has nowhere to go, which is why until
 * now running out of RAM was failure rather than degradation.
 *
 * This is a page-granular store on a raw block device. Not a swap FILE on
 * EMBKFS: the filesystem is copy-on-write and every write is a transaction,
 * and a page-out that commits a tree is the wrong cost for what is, by
 * definition, the memory nobody is using. A raw device is written in place.
 *
 * FOUND BY ITS HEADER, not by a partition type or a name: block 0 carries a
 * magic, the page size, and the slot count, written by tools/mkswap.py. The
 * kernel scans registered block devices for it at boot exactly as EMBKFS is
 * found by its superblock. A device without the header is never touched.
 *
 * A SLOT is one page. Slot 0 is the header and is never handed out.
 * ========================================================================== */

#define SWAP_MAGIC        "EMBKSWAP"
#define SWAP_HEADER_VER   1

struct swap_header {
    char     magic[8];        /* "EMBKSWAP"                                  */
    uint32_t version;
    uint32_t page_size;       /* 4096                                        */
    uint64_t nslots;          /* including slot 0, the header                */
    uint8_t  uuid[16];
    uint8_t  pad[4096 - 8 - 4 - 4 - 8 - 16];
};

/* Scan the block devices for a swap store. Returns the slots available, or 0
 * if none. Once, at boot, after the block layer is up. */
uint64_t swap_init(void);

bool     swap_available(void);

/* Write the page at physical `phys` to a free slot. Returns the slot (>= 1),
 * or 0 when the store is full or absent. Blocks on the device. */
uint64_t swap_out(uint64_t phys);

/* Read slot `slot` into the page at `phys`. The slot stays allocated until
 * swap_free(): a page that is swapped in but not yet dirtied could be dropped
 * again without a second write, which is a refinement this does not make yet
 * -- callers free the slot on swap-in. */
int      swap_in(uint64_t slot, uint64_t phys);

void     swap_free(uint64_t slot);

/* CLUSTERED PAGE-OUT: up to SWAP_CLUSTER_PAGES pages to CONTIGUOUS slots in
 * ONE device command. Measured before it was built: with one command per page
 * the store cost 158 us/page idle and ~350 us/page under load, and a run that
 * paged 190 MiB spent 26 of its 53 seconds waiting on those commands. The
 * per-command latency, not the bytes, is the cost -- so sixteen pages per
 * command is sixteen times fewer of it.
 *
 * All or nothing: returns n with slots_out[i] holding phys[i]'s slot, or 0
 * when no run of n free slots exists or the write failed, and the caller
 * falls back to swap_out() one page at a time. */
#define SWAP_CLUSTER_PAGES 16
int      swap_out_cluster(const uint64_t *phys, int n, uint64_t *slots_out);

/* Slots not in use. Reclaim asks BEFORE it unmaps a page to send here: a
 * full store should mean "no anonymous candidates", not "unmap, fail, refault". */
uint64_t swap_free_slots(void);

struct swap_stats {
    uint64_t nslots, used;
    uint64_t outs, ins, frees;
    uint64_t clusters;        /* multi-page writes issued                       */
    uint64_t cluster_pages;   /* pages that went out inside them (also in outs) */
    uint64_t bytes_written, bytes_read;
    char     dev[16];
};
void swap_stats_get(struct swap_stats *out);

#endif
