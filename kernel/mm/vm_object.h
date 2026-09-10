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
 * ===================================================================== */

struct vm_object;

/* One resident page of an object. */
struct vmo_page {
    uint64_t index;              /* page number WITHIN the object      */
    uint64_t phys;               /* the frame holding it               */
    bool     dirty;              /* differs from what is on the device */
    uint32_t wired;              /* mappings pinning it; never evicted while > 0 */
    struct vmo_page *hnext;      /* hash chain within the object       */
    struct vmo_page *lru_prev;   /* global LRU, newest at the head     */
    struct vmo_page *lru_next;
    struct vm_object *owner;     /* so the LRU can find its way home   */
};

#define VMO_HASH_BUCKETS 16

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
    struct vmo_page *buckets[VMO_HASH_BUCKETS];
    struct vm_object *reg_next;  /* registry chain                     */
};

/* Find (or create) the object for a file, taking a reference. Two callers
 * naming the same file get the SAME object -- that is the point. */
struct vm_object *vmo_get(struct vnode vn);

/* Drop a reference. The last one flushes the object's dirty pages and frees
 * its frames: a cache that outlived every descriptor onto it would hold
 * memory nothing can reach. */
void vmo_put(struct vm_object *o);

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

/* Give back up to `want` pages to the physical allocator, cleanest first.
 * This is what makes the cache bounded: it grows into free memory and yields
 * it the moment anything else needs it, which is the whole reason to cache
 * aggressively in the first place. Returns the number actually freed. */
uint64_t vmo_reclaim(uint64_t want);

/* Cache-wide counters, for `test pagecache` and the memory display. */
struct vmo_stats {
    uint64_t objects;
    uint64_t resident_pages;
    uint64_t dirty_pages;
    uint64_t hits;               /* reads served without touching the device */
    uint64_t misses;             /* reads that had to fetch                  */
    uint64_t writebacks;         /* pages pushed to the device               */
    uint64_t evictions;          /* pages reclaimed under pressure           */
};
void vmo_stats_get(struct vmo_stats *out);

/* Start the writeback thread. After the scheduler and the VFS. */
void vmo_writeback_init(void);

#endif /* _VM_OBJECT_H_ */
