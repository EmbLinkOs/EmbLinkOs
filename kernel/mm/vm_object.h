#ifndef _VM_OBJECT_H_
#define _VM_OBJECT_H_

#include <stdint.h>
#include "include/types.h"
#include "fs/vfs.h"

/* =====================================================================
 * VM OBJECTS -- ONE owner for a file's resident pages.
 *
 * THE DECISION THIS FILE MAKES, and it is the one that shapes everything
 * built on top of it: there is no separate "buffer cache" and "page cache".
 * There is ONE object per file, it owns the physical frames, and every path
 * that wants those bytes goes through it -- read(), write(), mmap(), the
 * writeback thread, and whatever compressor or swap pager comes later.
 *
 * The alternative -- a block cache under the filesystem and a page cache over
 * it -- is two copies of the same bytes that can disagree. A program that
 * mmap()s a file and another that read()s it would see different data, and
 * neither would be wrong on its own terms. Unifying them is not an
 * optimisation; it is what makes the two APIs describe the same file.
 *
 * WHAT THE KERNEL DID BEFORE THIS: every write() went straight to the device,
 * synchronously, and every read() re-read it. Opening a file twice read it
 * twice. Appending one byte to a file rewrote the object through a full
 * copy-on-write transaction -- so a program writing a log a line at a time
 * committed a transaction per line. fsync() was "vacuously true" because
 * there was nothing to flush. That last part is now false, which is why
 * fsync() had a standing obligation attached to it: the day a write-back
 * cache lands, fsync must become a real flush. It does, in the same commit.
 *
 * WHAT THIS COSTS, stated plainly: a write() that returns has NOT reached the
 * device. A crash between the write and its writeback loses it. That is the
 * bargain every serious operating system makes, and the tools to control it
 * are the ones POSIX names -- fsync() for "I need this on disk now", and a
 * bounded writeback interval so nothing dirty lingers indefinitely. What is
 * NOT acceptable, and does not happen here, is a write() that reports success
 * for data no one is tracking.
 *
 * OBJECT IDENTITY IS THE FILE, not the descriptor. Two opens of one file find
 * the SAME object through the registry, so they share its pages and its
 * cached size. Without that, two descriptors would each cache their own copy
 * and the second writer's data would be invisible to the first reader --
 * exactly the incoherence the unified design exists to prevent.
 *
 * ANONYMOUS MEMORY IS AN OBJECT TOO -- one with no file behind it. That is
 * what lets it be paged: the swap store (mm/swap.h) is the backing store an
 * anonymous object writes to and reads from, exactly as the filesystem is for
 * a file object, and reclaim treats the two by one rule -- a page whose
 * contents exist somewhere else is dropped, a page whose contents exist only
 * here is written out first. Before this, mmap(MAP_ANONYMOUS) pages were bare
 * frames the fault handler allocated and nothing tracked; they could not be
 * found, counted or evicted, so an anonymous mapping was memory that had to
 * fit or fail. Now every mapping has an object, and running out of memory
 * degrades instead of failing.
 * ===================================================================== */

struct vm_object;

/* One resident page of an object. */
struct vmo_page {
    uint64_t index;              /* page number WITHIN the object      */
    uint64_t phys;               /* the frame holding it -- 0 while it is out on swap */
    uint64_t swap_slot;          /* where it is when phys is 0 (anonymous objects)   */
    bool     dirty;              /* differs from what is on the device */
    uint32_t wired;              /* mappings pinning it; never evicted while > 0 */
    struct vmo_page *hnext;      /* hash chain within the object       */
    struct vmo_page *lru_prev;   /* global LRU, newest at the head     */
    struct vmo_page *lru_next;
    struct vm_object *owner;     /* so the LRU can find its way home   */
};

/* The per-object page index: a chained hash on the page number that DOUBLES
 * whenever it holds more pages than buckets, so a lookup stays O(1) whether
 * the object is a three-page file or a sixty-thousand-page anonymous mapping.
 * Sixteen inline buckets cover the common small object with no allocation.
 * The first version stopped at sixteen, and a paging run over one 229 MiB
 * object walked ~3,600-entry chains on every fault -- 485 us per fault, most
 * of it pointer chasing. */
#define VMO_HASH_BUCKETS      16
#define VMO_HASH_MAX_BUCKETS  (1u << 16)

struct vm_object {
    struct vnode vn;             /* the file this caches               */
    uint64_t size;               /* AUTHORITATIVE size, including bytes
                                  * only in the cache. stat() consults
                                  * it: a deferred write that had not
                                  * reached the device would otherwise
                                  * make the file look shorter than the
                                  * data a reader can already see. */
    int      refs;               /* open file descriptions holding it  */
    uint32_t resident;           /* pages held                         */
    uint32_t dirty_pages;
    bool     no_writeback;       /* the file is gone; drop, never flush */

    /* ANONYMOUS OBJECTS. No vnode, no writeback; the swap store is the backing.
     *
     * ONE MAPPER. A file object is mapped by any number of address spaces and
     * knows none of them -- the wire count is all it sees, and it never has to
     * unmap anything itself, because a file page's contents are on the device
     * already. An anonymous page's contents are ONLY in its frame, so evicting
     * it means taking it out of the page table that maps it FIRST (or a store
     * through the stale PTE lands in a frame that is already on its way to
     * disk). That needs the address space and the address. An anonymous object
     * is private to the process that mapped it, so there is exactly one, and
     * it is recorded here rather than in a reverse map the kernel does not
     * otherwise need. Shared anonymous memory (MAP_SHARED|MAP_ANONYMOUS) would
     * need the rmap; it is refused today, and this is why. */
    bool     anon;
    uint64_t mapper_pml4;        /* the address space that maps it, 0 if none yet */
    uint64_t mapper_base;        /* the VA of page 0 in that address space        */
    uint32_t swapped;            /* pages out on the swap store                   */

    struct vmo_page **buckets;   /* nbuckets entries; inline_buckets until it grows */
    uint32_t nbuckets;           /* a power of two                     */
    struct vmo_page *inline_buckets[VMO_HASH_BUCKETS];
    struct vm_object *reg_next;  /* registry chain                     */
};

/* Find (or create) the object for a file, taking a reference. Two callers
 * naming the same file get the SAME object -- that is the point. */
struct vm_object *vmo_get(struct vnode vn);

/* Drop a reference. The last one flushes the object's dirty pages and frees
 * its frames: a cache that outlived every descriptor onto it would hold
 * memory nothing can reach. */
void vmo_put(struct vm_object *o);

/* A second holder of an object already held -- what a mapping that is SPLIT
 * (munmap of its middle, mprotect of part of it) gives the new half. Not
 * vmo_get(): that finds a FILE's object by its vnode, and an anonymous object
 * has none. */
void vmo_ref(struct vm_object *o);

/* --- anonymous objects ----------------------------------------------------
 *
 * An object with no file: `size` bytes of zero-filled, swappable memory,
 * holding one reference (the mapping's). Pages are filled with zeroes on first
 * touch -- a security property, not a courtesy: a frame carries whatever its
 * last owner left in it -- and are written to the swap store under pressure
 * and read back by the fault that next wants them. */
struct vm_object *vmo_create_anon(uint64_t size);

/* Record the ONE address space that maps an anonymous object and the VA of
 * its page 0 there. Required before the object can be swapped; see the `anon`
 * block in struct vm_object for why the object has to know. */
void vmo_set_mapper(struct vm_object *o, uint64_t pml4_phys, uint64_t base_va);

/* Part of an anonymous object is being unmapped, for good. Unmap the pages in
 * [first, first+count) from the mapper, free their frames or swap slots, and
 * forget them. O(pages that exist in the range), not O(range). */
void vmo_discard_range(struct vm_object *o, uint64_t first, uint64_t count);

/* The mapper is going away (process exit). Take every resident page out of
 * its page tables and release the wires, so the address-space teardown that
 * frees every frame it finds does not free the object's. */
void vmo_detach_mapper(struct vm_object *o);

/* Byte-range I/O through the cache. `read` faults absent pages in from the
 * filesystem; `write` marks them dirty and returns without touching the
 * device. Both return EMBK_OK and set *out_done, or a negative error. */
int vmo_read(struct vm_object *o, uint64_t off, void *buf, uint64_t len,
             uint64_t *out_done);
int vmo_write(struct vm_object *o, uint64_t off, const void *buf, uint64_t len,
              uint64_t *out_done);

/* Push every dirty page of this object to the device. What fsync() is. */
int vmo_flush(struct vm_object *o);

/* The file's size as the cache knows it -- which INCLUDES bytes that have not
 * reached the device. stat() and SEEK_END both need this: reporting the
 * device's length would make stat() disagree with read(), and would make an
 * append loop seek back over its own unflushed data. */
uint64_t vmo_size(struct vm_object *o);

/* The file was truncated to `size` behind the cache's back (ftruncate,
 * O_TRUNC): drop the pages past the new end and adopt the size. Not doing
 * this is a stale-tail bug -- the cache would happily serve the old bytes
 * of a file that no longer has them.
 *
 * The `_file` form takes the file's IDENTITY rather than a held object,
 * because the truncating descriptor is not necessarily the one that has the
 * pages -- another process may have the file open. It is a no-op if nothing
 * is cached. */
void vmo_truncated(struct vm_object *o, uint64_t size);
void vmo_file_truncated(struct vnode vn, uint64_t size);

/* The file is being unlinked. Its dirty pages must NOT be written back:
 * the object they belong to may already be gone, and writing to a freed
 * object id is corruption of whatever now owns those blocks. */
void vmo_invalidate(struct vm_object *o);

/* Flush every dirty page in the SYSTEM. Called by the writeback thread on its
 * interval, and before anything that must see a consistent device (unmount,
 * shutdown). Returns the number of pages written. */
uint64_t vmo_writeback_all(void);

/* Give back up to `want` pages to the physical allocator, cheapest first:
 * clean file pages (cost: a re-read), then dirty file pages (a writeback),
 * then anonymous pages (a write to the swap store and a read to come back).
 * This is what makes the cache bounded -- it grows into free memory and yields
 * it the moment anything else needs it -- and, since anonymous memory became
 * an object, what makes running out of memory a slowdown rather than a
 * failure. Returns the number actually freed. */
uint64_t vmo_reclaim(uint64_t want);

/* --- mapping a cached page into an address space --------------------------
 *
 * THIS IS THE PAYOFF for building the cache as an object rather than as a
 * cache. Mapping a file is not "read it into some pages" -- it is handing the
 * process the pages the object ALREADY HOLDS. No copy, no second
 * implementation, and no way for a reader and a mapper to disagree about what
 * the file says, because there is only one set of pages.
 *
 * `vmo_wire_page` faults the page in if absent and PINS it: a pinned page is
 * skipped by the reclaimer, because evicting a frame that is live in some
 * process's page table would hand that process someone else's memory. Returns
 * the frame, or 0.
 *
 * `vmo_unwire_page` releases the pin. A page that was never wired is left
 * alone rather than under-flowing the count.
 *
 * `vmo_page_phys` answers "which frame backs this index, if any" WITHOUT
 * faulting or pinning -- what munmap uses to tell a pinned cache page from a
 * private copy-on-write copy it has to free itself. */
uint64_t vmo_wire_page(struct vm_object *o, uint64_t index);
void     vmo_unwire_page(struct vm_object *o, uint64_t index);
uint64_t vmo_page_phys(struct vm_object *o, uint64_t index);

/* Mark a wired page dirty -- a MAP_SHARED write reaches the file through the
 * ordinary writeback path, which is the whole point of MAP_SHARED. The kernel
 * cannot see the store, so the mapper says so when it maps for writing. */
void vmo_mark_dirty(struct vm_object *o, uint64_t index);

/* Cache-wide counters, for `test pagecache` and the memory display. */
struct vmo_stats {
    uint64_t objects;
    uint64_t resident_pages;
    uint64_t dirty_pages;
    uint64_t hits;               /* reads served without touching the device */
    uint64_t misses;             /* reads that had to fetch                  */
    uint64_t writebacks;         /* pages pushed to the device               */
    uint64_t evictions;          /* pages reclaimed under pressure           */
    uint64_t anon_objects;
    uint64_t anon_pages;         /* anonymous pages resident (NOT in resident_pages) */
    uint64_t swapped_pages;      /* anonymous pages out on the swap store    */
    uint64_t swapouts;           /* anonymous pages written to the store     */
    uint64_t swapins;            /* ... and read back                        */
    uint64_t readahead_pages;    /* pages brought back BEFORE they faulted   */
    uint64_t readahead_reads;    /* device commands that did it              */

    /* Where reclaim spends its time. Kept, not just for one measurement,
     * because these are the numbers that decide the next change here. */
    uint64_t reclaim_calls;
    uint64_t lru_visited;        /* LRU nodes examined across all calls      */
    uint64_t reclaim_ns;         /* wall time inside the reclaimer           */
    uint64_t swap_ns;            /* ... of which, in the swap store (out+in) */
    uint64_t unmap_ns;           /* ... of which, unmapping (TLB shootdowns) */

    /* Where a page FILL spends its time: the frame, zeroing it, its record. */
    uint64_t fill_frame_ns;
    uint64_t fill_zero_ns;
    uint64_t fill_record_ns;
};
void vmo_stats_get(struct vmo_stats *out);

/* AUDIT the swap bookkeeping: every anonymous page that is out on the store
 * must name a slot the store holds, and no two pages may name the same slot.
 * Returns the number of pages out; *dangling counts slots the store does not
 * hold, *duplicates the pages sharing a slot with an earlier one. Walks every
 * object under the cache lock: a test's tool, not a fast path. */
uint64_t vmo_audit_swap(uint64_t *store_used, uint64_t *dangling, uint64_t *duplicates);

/* Start the writeback thread. After the scheduler and the VFS. */
void vmo_writeback_init(void);

#endif /* _VM_OBJECT_H_ */
