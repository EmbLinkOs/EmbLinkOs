#include "mm/vm_object.h"
#include "mm/pmm.h"
#include "mm/vmm.h"          /* vmm_unmap_in: taking an anonymous page out of its mapper */
#include "mm/swap.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "process/process.h"
#include "process/ksync.h"
#include "drivers/timer/timer.h"

/* See mm/vm_object.h for what this is and why it is one object rather than
 * two caches. This file is the mechanism. */

/* ONE lock for the whole cache: the registry, every object's page set, and
 * the global LRU. A SLEEPING lock, because the fill and flush paths call into
 * the filesystem and the filesystem talks to a disk.
 *
 * One lock is the wrong long-term answer and the right first one. Per-object
 * locking plus a separate LRU lock is the shape this wants eventually, and
 * the moment to build it is when a profile shows contention rather than when
 * it sounds better -- the cost of getting a two-lock ordering wrong here is a
 * deadlock in the path every file read takes. Recorded in docs/TODO.md. */
static struct mutex g_lock;
static bool g_ready = false;

static struct vm_object *g_registry = NULL;

/* TWO LRUs, one for file pages and one for anonymous pages, each newest at the
 * head. Reclaim walks a list from its TAIL -- the least recently used page of
 * that kind in the system, regardless of which object it belongs to; per-object
 * LRUs would let a file read once and never again hold pages a hot file needs.
 *
 * Two lists rather than one because reclaim's passes are BY KIND: clean file
 * pages first, then dirty ones, then anonymous pages to the swap store. With
 * one list the first pass walked every anonymous node to find none of them
 * evictable -- measured at 35,000 nodes per call, 3.4 million in one paging
 * run -- before the pass that wanted them started over from the tail. */
struct lru { struct vmo_page *head, *tail; };
static struct lru g_lru_file, g_lru_anon;

static struct vmo_stats g_stats;

/* How large the cache may grow, as a fraction of the free memory the system
 * booted with. The cache is supposed to consume idle memory -- memory the
 * system is not otherwise using is memory being wasted -- but it must yield
 * it instantly, which is what vmo_reclaim is for. The cap is a backstop so a
 * runaway does not have to be caught by the reclaimer under pressure. */
static uint64_t g_max_resident = 0;

/* Coalescing buffer for writeback. Contiguous dirty pages are gathered here
 * and written in ONE filesystem call, because a filesystem write is not cheap
 * per call: EMBKFS commits a copy-on-write transaction, so flushing sixteen
 * adjacent pages one at a time is sixteen transactions describing the same
 * file. Guarded by g_lock like everything else here. */
#define VMO_FLUSH_RUN_PAGES 16
static uint8_t g_flush_buf[VMO_FLUSH_RUN_PAGES * PAGE_SIZE];

static inline void *page_va(uint64_t phys) {
    return (void *)(uintptr_t)P2V(phys);
}

/* ---- LRU ---------------------------------------------------------------- */

static inline struct lru *lru_of(struct vmo_page *p) {
    return p->owner->anon ? &g_lru_anon : &g_lru_file;
}

static void lru_unlink(struct vmo_page *p) {
    struct lru *l = lru_of(p);
    if (p->lru_prev) p->lru_prev->lru_next = p->lru_next;
    else if (l->head == p) l->head = p->lru_next;
    if (p->lru_next) p->lru_next->lru_prev = p->lru_prev;
    else if (l->tail == p) l->tail = p->lru_prev;
    p->lru_prev = p->lru_next = NULL;
}

static void lru_touch(struct vmo_page *p) {
    struct lru *l = lru_of(p);
    if (l->head == p)
        return;
    lru_unlink(p);
    p->lru_next = l->head;
    if (l->head) l->head->lru_prev = p;
    l->head = p;
    if (!l->tail) l->tail = p;
}

/* ---- page records ------------------------------------------------------- */

/* PAGE RECORDS COME FROM A POOL, NOT FROM kmalloc. Measured: the kernel heap's
 * slab pools are capped at 128 regions of 8 KiB -- one megabyte in all -- and
 * past that every small allocation falls to the general first-fit heap, which
 * walks every block before it. A paging run that filled 58,624 pages spent
 * 13.2 s of its 36 s allocating their records: 224 us per kmalloc. The page
 * cache is the one thing in the kernel that wants tens of thousands of small
 * objects, so it carries them itself, the way every kernel carries its page
 * structures: chunks of records threaded on a free list, O(1) either way.
 *
 * Chunks come from the heap ARENA (kmalloc), not from the frame allocator --
 * so caching a file or paging a mapping never changes the free-frame count
 * by anything but the pages themselves, which is what `test mmap`'s exact
 * round trip asserts. Chunks are kept, not returned: the pool's high-water
 * mark is a few dozen bytes per page ever cached at once, the ratio every
 * kernel pays for its page array. */
#define VMO_RECORD_CHUNK_BYTES (32u * 1024u)
static struct vmo_page *g_rec_free;          /* free list, threaded through hnext */
static uint64_t g_rec_chunks, g_rec_free_count;

static struct vmo_page *record_alloc(void) {
    if (!g_rec_free) {
        struct vmo_page *chunk = kmalloc(VMO_RECORD_CHUNK_BYTES);
        if (!chunk)
            return NULL;
        uint64_t n = VMO_RECORD_CHUNK_BYTES / sizeof *chunk;
        for (uint64_t i = 0; i < n; i++) {
            chunk[i].hnext = g_rec_free;
            g_rec_free = &chunk[i];
        }
        g_rec_chunks++;
        g_rec_free_count += n;
    }
    struct vmo_page *p = g_rec_free;
    g_rec_free = p->hnext;
    g_rec_free_count--;
    return p;
}

static void record_free(struct vmo_page *p) {
    p->hnext = g_rec_free;
    g_rec_free = p;
    g_rec_free_count++;
}

/* ---- pages within an object --------------------------------------------- */

static inline uint32_t bucket_of(const struct vm_object *o, uint64_t index) {
    return (uint32_t)(index & (o->nbuckets - 1));
}

static struct vmo_page *page_find(struct vm_object *o, uint64_t index) {
    for (struct vmo_page *p = o->buckets[bucket_of(o, index)]; p; p = p->hnext)
        if (p->index == index)
            return p;
    return NULL;
}

static void page_unlink(struct vm_object *o, struct vmo_page *p) {
    struct vmo_page **pp = &o->buckets[bucket_of(o, p->index)];
    while (*pp && *pp != p)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = p->hnext;
}

/* Double the index when it holds more pages than buckets. Amortised O(1) per
 * insert. A failed allocation keeps the old table -- slower, not wrong. Caller
 * holds g_lock. */
static void index_grow_if_needed(struct vm_object *o) {
    uint64_t pages = (uint64_t)o->resident + o->swapped;
    if (pages <= o->nbuckets || o->nbuckets >= VMO_HASH_MAX_BUCKETS)
        return;
    uint32_t nb = o->nbuckets * 2;
    struct vmo_page **nt = kmalloc((uint64_t)nb * sizeof *nt);
    if (!nt)
        return;
    memset(nt, 0, (size_t)nb * sizeof *nt);
    for (uint32_t b = 0; b < o->nbuckets; b++) {
        struct vmo_page *p = o->buckets[b];
        while (p) {
            struct vmo_page *next = p->hnext;
            uint32_t h = (uint32_t)(p->index & (nb - 1));
            p->hnext = nt[h];
            nt[h] = p;
            p = next;
        }
    }
    if (o->buckets != o->inline_buckets)
        kfree(o->buckets);
    o->buckets  = nt;
    o->nbuckets = nb;
}

/* Release one page: its frame goes back to the allocator and the record with
 * it. The caller must have flushed it if it was dirty and the data mattered. */
static void page_destroy(struct vm_object *o, struct vmo_page *p) {
    lru_unlink(p);
    page_unlink(o, p);
    if (p->dirty && o->dirty_pages) o->dirty_pages--;
    /* A page is in a frame OR in a swap slot -- never both, never neither. */
    if (p->phys) {
        o->resident--;
        pmm_free_page(p->phys);
    } else if (p->swap_slot) {
        if (o->swapped) o->swapped--;
        swap_free(p->swap_slot);
    }
    record_free(p);
}

/* Fetch page `index` from the filesystem into a fresh frame.
 *
 * A short read is not an error: it is the end of the file, and the rest of the
 * page must be ZERO rather than whatever the frame held. A page served with a
 * previous owner's bytes in its tail is a disclosure, and it is one that only
 * shows up on files whose length is not a multiple of the page size -- which
 * is almost all of them. */
static uint64_t reclaim_locked(uint64_t want, bool swap_ok);

/* A frame for a page, making room if there is none.
 *
 * THIS IS WHERE PRESSURE BECOMES PAGING. The writeback thread reclaims when
 * free memory falls under its watermark, but it wakes ten times a second and
 * a process touching pages in a loop is faster than that. So the fault path
 * reclaims for itself when the allocator is nearly dry -- from the cache it is
 * standing in, under the lock it already holds -- and then allocates.
 *
 * A RESERVE, not "on failure". The page TABLES this page needs are allocated
 * by vmm_map_in from the same pool, and so is every kmalloc, and neither can
 * reclaim. Keeping a margin means the fault that paged this page in does not
 * then fail on the table that maps it. */
/* 256 pages of reserve and a batch of 512 -- 32 clusters -- rather than the
 * 128/64 this started with. With the small numbers the fault path kept free
 * memory hovering just above its own reserve under sustained pressure, and
 * the read-ahead below, which needs room for a cluster and never reclaims to
 * get it, found none: 349 pages read ahead of 18,750 swapped in. */
#define VMO_FAULT_RESERVE_PAGES  256     /* 1 MiB */
#define VMO_FAULT_RECLAIM_BATCH  512
static uint64_t frame_or_reclaim(void) {
    if (pmm_free_pages() < VMO_FAULT_RESERVE_PAGES)
        (void)reclaim_locked(VMO_FAULT_RECLAIM_BATCH, true);
    uint64_t phys = pmm_alloc_page();
    if (phys)
        return phys;
    /* The batch did not cover it, or the allocator refused above the reserve
     * (fragmentation is not a thing for single pages, so: it is truly empty).
     * Once more, then the caller hears about it. */
    (void)reclaim_locked(VMO_FAULT_RECLAIM_BATCH, true);
    return pmm_alloc_page();
}

static struct vmo_page *page_fill(struct vm_object *o, uint64_t index, bool zero_only) {
    uint64_t t0 = time_get_ns();
    uint64_t phys = frame_or_reclaim();
    uint64_t t1 = time_get_ns();
    g_stats.fill_frame_ns += t1 - t0;
    if (!phys)
        return NULL;

    uint8_t *va = (uint8_t *)page_va(phys);
    memset(va, 0, PAGE_SIZE);
    uint64_t t2 = time_get_ns();
    g_stats.fill_zero_ns += t2 - t1;

    if (!zero_only && o->vn.mnt && o->vn.mnt->ops && o->vn.mnt->ops->read) {
        uint64_t off = index * PAGE_SIZE;
        if (off < o->size) {
            size_t got = 0;
            uint64_t want = o->size - off;
            if (want > PAGE_SIZE) want = PAGE_SIZE;
            int rc = o->vn.mnt->ops->read(&o->vn, off, va, (size_t)want, &got);
            if (rc != EMBK_OK) {
                pmm_free_page(phys);
                return NULL;
            }
            if (got < want)
                memset(va + got, 0, (size_t)(want - got));
        }
    }

    uint64_t t3 = time_get_ns();
    struct vmo_page *p = record_alloc();
    g_stats.fill_record_ns += time_get_ns() - t3;
    if (!p) {
        pmm_free_page(phys);
        return NULL;
    }
    p->index = index;
    p->phys  = phys;
    p->dirty = false;
    p->wired = 0;
    p->swap_slot = 0;
    p->owner = o;
    p->lru_prev = p->lru_next = NULL;
    p->hnext = o->buckets[bucket_of(o, index)];
    o->buckets[bucket_of(o, index)] = p;
    o->resident++;
    lru_touch(p);
    index_grow_if_needed(o);
    return p;
}

/* READ-AHEAD AROUND A SWAP-IN. The pages that left RAM in one cluster sit in
 * consecutive slots, and a program that touched them together will very
 * likely touch them together again -- the witness's reverse pass faulted on
 * every one of them, one 380 us read at a time. So after a page comes back,
 * its neighbours in the object that are also out on the store are fetched
 * too, one command per run of contiguous slots, and are simply RESIDENT when
 * the fault for them arrives: a hit, no I/O.
 *
 * Bounded on purpose: a window of VMO_RA_EACH_SIDE pages each way (one
 * cluster in all), and only when the allocator is comfortably above the
 * fault path's reserve -- read-ahead never reclaims, since evicting a page
 * somebody is using to prefetch one somebody might use is a bad trade. A
 * frame that cannot be had ends the read-ahead, not the fault. Caller holds
 * g_lock. */
#define VMO_RA_EACH_SIDE 8
static void swap_readahead_locked(struct vm_object *o, uint64_t index) {
    if (pmm_free_pages() < VMO_FAULT_RESERVE_PAGES + 2 * VMO_RA_EACH_SIDE)
        return;

    /* Candidates: swapped-out neighbours, sorted by slot. */
    struct vmo_page *cand[2 * VMO_RA_EACH_SIDE];
    int n = 0;
    for (uint64_t d = 1; d <= VMO_RA_EACH_SIDE; d++) {
        uint64_t idx[2] = { index + d, index - d };
        for (int k = 0; k < 2; k++) {
            if (k == 1 && index < d) continue;
            struct vmo_page *p = page_find(o, idx[k]);
            if (p && !p->phys && p->swap_slot)
                cand[n++] = p;
        }
    }
    for (int i = 1; i < n; i++) {                  /* insertion sort: n <= 16 */
        struct vmo_page *p = cand[i]; int j = i - 1;
        while (j >= 0 && cand[j]->swap_slot > p->swap_slot) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = p;
    }

    /* Each run of contiguous slots is one read. */
    for (int i = 0; i < n; ) {
        int run = 1;
        while (i + run < n && cand[i + run]->swap_slot == cand[i]->swap_slot + (uint64_t)run) run++;

        uint64_t phys[2 * VMO_RA_EACH_SIDE];
        int got = 0;
        for (; got < run; got++) {
            phys[got] = pmm_alloc_page();
            if (!phys[got]) break;
        }
        if (got < run) {                            /* out of frames: stop reading ahead */
            for (int k = 0; k < got; k++) pmm_free_page(phys[k]);
            return;
        }

        uint64_t ts = time_get_ns();
        int rc = (run == 1) ? swap_in(cand[i]->swap_slot, phys[0])
                            : swap_in_cluster(cand[i]->swap_slot, run, phys);
        g_stats.swap_ns += time_get_ns() - ts;
        if (rc != EMBK_OK) {
            for (int k = 0; k < run; k++) pmm_free_page(phys[k]);
            return;
        }
        for (int k = 0; k < run; k++) {
            struct vmo_page *p = cand[i + k];
            swap_free(p->swap_slot);
            p->swap_slot = 0;
            p->phys = phys[k];
            o->resident++;
            if (o->swapped) o->swapped--;
            g_stats.swapins++;
            g_stats.readahead_pages++;
            lru_touch(p);
        }
        g_stats.readahead_reads++;
        i += run;
    }
}

/* The page for `index`, faulting it in if absent. */
static struct vmo_page *page_get(struct vm_object *o, uint64_t index, bool zero_only) {
    struct vmo_page *p = page_find(o, index);
    if (p && !p->phys) {
        /* KNOWN, BUT OUT ON THE SWAP STORE. The record survived eviction
         * precisely so that this lookup would find the slot. Bring the page
         * back into a fresh frame. The slot is released on the way in; a page
         * that comes back and is later dropped unchanged could have kept it,
         * which is the refinement swap.h notes and this does not make. */
        uint64_t phys = frame_or_reclaim();
        if (!phys)
            return NULL;
        uint64_t ts = time_get_ns();
        int rc = swap_in(p->swap_slot, phys);
        g_stats.swap_ns += time_get_ns() - ts;
        if (rc != EMBK_OK) {
            kprintf("pagecache: swap-in of slot %llu failed (%d); the fault is declined\n",
                    (unsigned long long)p->swap_slot, rc);
            pmm_free_page(phys);
            return NULL;
        }
        swap_free(p->swap_slot);
        p->swap_slot = 0;
        p->phys = phys;
        o->resident++;
        if (o->swapped) o->swapped--;
        g_stats.swapins++;
        g_stats.misses++;
        lru_touch(p);
        if (o->anon)
            swap_readahead_locked(o, index);
        return p;
    }
    if (p) {
        g_stats.hits++;
        lru_touch(p);
        return p;
    }
    g_stats.misses++;
    return page_fill(o, index, zero_only);
}

/* ---- writeback ---------------------------------------------------------- */

/* Push one run of contiguous dirty pages starting at `first`. Returns how many
 * pages were written, 0 on error. */
static uint32_t flush_run(struct vm_object *o, uint64_t first) {
    if (!o->vn.mnt || !o->vn.mnt->ops || !o->vn.mnt->ops->write)
        return 0;

    uint32_t n = 0;
    struct vmo_page *run[VMO_FLUSH_RUN_PAGES];
    while (n < VMO_FLUSH_RUN_PAGES) {
        struct vmo_page *p = page_find(o, first + n);
        if (!p || !p->dirty || !p->phys)
            break;
        run[n] = p;
        memcpy(g_flush_buf + (uint64_t)n * PAGE_SIZE, page_va(p->phys), PAGE_SIZE);
        n++;
    }
    if (n == 0)
        return 0;

    uint64_t off = first * PAGE_SIZE;
    uint64_t len = (uint64_t)n * PAGE_SIZE;

    /* Never write past the end of the file. The last page of a file is only
     * partly meaningful, and writing its whole 4 KiB would silently extend the
     * file to a page boundary -- a file that grew by up to 4095 zero bytes
     * every time it was flushed. */
    if (off + len > o->size)
        len = (off < o->size) ? (o->size - off) : 0;
    if (len == 0)
        return 0;

    size_t wrote = 0;
    int rc = o->vn.mnt->ops->write(&o->vn, off, g_flush_buf, (size_t)len, &wrote);
    if (rc != EMBK_OK)
        return 0;

    for (uint32_t i = 0; i < n; i++) {
        if (run[i]->dirty) {
            run[i]->dirty = false;
            if (o->dirty_pages) o->dirty_pages--;
            g_stats.dirty_pages = g_stats.dirty_pages ? g_stats.dirty_pages - 1 : 0;
            g_stats.writebacks++;
        }
    }
    return n;
}

/* Flush every dirty page of one object, lowest index first.
 *
 * Ascending order is not cosmetic: a filesystem that appends is far happier
 * receiving a file's pages in the order the file is laid out, and EMBKFS's
 * extent builder in particular turns a forward run into one extent and a
 * shuffled one into many. */
static int flush_object_locked(struct vm_object *o) {
    if (o->no_writeback || o->dirty_pages == 0)
        return EMBK_OK;

    uint64_t last = (o->size + PAGE_SIZE - 1) / PAGE_SIZE;
    int rc = EMBK_OK;

    for (uint64_t i = 0; i < last && o->dirty_pages > 0; ) {
        struct vmo_page *p = page_find(o, i);
        if (!p || !p->dirty) { i++; continue; }
        uint32_t n = flush_run(o, i);
        if (n == 0) { rc = -EMBK_EIO; break; }
        i += n;
    }
    return rc;
}

/* ---- objects ------------------------------------------------------------ */

static void obj_free_pages_locked(struct vm_object *o) {
    uint64_t frames = 0, slots = 0, neither = 0;
    for (uint32_t b = 0; b < o->nbuckets; b++) {
        struct vmo_page *p = o->buckets[b];
        while (p) {
            struct vmo_page *next = p->hnext;
            lru_unlink(p);
            if (p->phys)           { pmm_free_page(p->phys); frames++; }
            else if (p->swap_slot) { swap_free(p->swap_slot); slots++; }
            else neither++;
            record_free(p);
            p = next;
        }
        o->buckets[b] = NULL;
    }
    (void)frames; (void)slots; (void)neither;
    o->resident = 0;
    o->dirty_pages = 0;
    o->swapped = 0;
}

static struct vm_object *registry_find(struct vnode vn) {
    for (struct vm_object *o = g_registry; o; o = o->reg_next)
        if (!o->anon && o->vn.mnt == vn.mnt && o->vn.ino == vn.ino)
            return o;
    return NULL;
}

static void registry_remove(struct vm_object *o) {
    struct vm_object **pp = &g_registry;
    while (*pp && *pp != o)
        pp = &(*pp)->reg_next;
    if (*pp)
        *pp = o->reg_next;
}

/* The cache only makes sense once there is a scheduler to sleep on the lock
 * and a filesystem to fill from. Before that -- and for any backing that is
 * not a real file -- callers fall back to the direct path. */
static bool cache_usable(struct vnode vn) {
    return g_ready && vn.mnt && vn.mnt->ops && vn.mnt->ops->read;
}

struct vm_object *vmo_get(struct vnode vn) {
    if (!cache_usable(vn))
        return NULL;

    mutex_lock(&g_lock);
    struct vm_object *o = registry_find(vn);
    if (o) {
        __atomic_fetch_add(&o->refs, 1, __ATOMIC_RELAXED);
        mutex_unlock(&g_lock);
        return o;
    }

    o = kmalloc(sizeof *o);
    if (!o) {
        mutex_unlock(&g_lock);
        return NULL;
    }
    memset(o, 0, sizeof *o);
    o->buckets  = o->inline_buckets;
    o->nbuckets = VMO_HASH_BUCKETS;
    o->vn = vn;
    o->refs = 1;

    /* The size comes from the filesystem ONCE, here. From this point the
     * object's own size is authoritative -- a write that has only reached the
     * cache has already changed the file as far as every reader is concerned,
     * and asking the device would report the old length. */
    if (vn.mnt->ops->stat) {
        struct vfs_stat st;
        if (vn.mnt->ops->stat(&vn, &st) == EMBK_OK)
            o->size = st.size;
    }

    o->reg_next = g_registry;
    g_registry = o;
    g_stats.objects++;
    mutex_unlock(&g_lock);
    return o;
}

void vmo_put(struct vm_object *o) {
    if (!o)
        return;
    mutex_lock(&g_lock);
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) > 0) {
        mutex_unlock(&g_lock);
        return;
    }

    /* Last descriptor gone. Flush before dropping: a dirty page whose object
     * no one can name again is data that was accepted and then discarded. */
    (void)flush_object_locked(o);
    obj_free_pages_locked(o);
    registry_remove(o);
    if (o->anon) { if (g_stats.anon_objects) g_stats.anon_objects--; }
    else if (g_stats.objects) g_stats.objects--;
    mutex_unlock(&g_lock);
    if (o->buckets != o->inline_buckets)
        kfree(o->buckets);
    kfree(o);
}

void vmo_ref(struct vm_object *o) {
    if (!o)
        return;
    /* NO LOCK, on purpose: the fault path calls this under the VMA spinlock,
     * where sleeping on g_lock would be sleeping with interrupts off. It is
     * safe because every caller already holds the object through something
     * that holds a reference -- a VMA it found under vma_lock -- so refs is
     * at least 1 and the last-reference teardown in vmo_put cannot be running. */
    __atomic_fetch_add(&o->refs, 1, __ATOMIC_RELAXED);
}

int vmo_read(struct vm_object *o, uint64_t off, void *buf, uint64_t len,
             uint64_t *out_done) {
    if (!o || !buf || !out_done)
        return -EMBK_EINVAL;
    *out_done = 0;

    mutex_lock(&g_lock);
    if (off >= o->size) {              /* EOF: zero bytes, not an error */
        mutex_unlock(&g_lock);
        return EMBK_OK;
    }
    if (off + len > o->size)
        len = o->size - off;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t done = 0;
    int rc = EMBK_OK;

    while (done < len) {
        uint64_t index = (off + done) / PAGE_SIZE;
        uint64_t within = (off + done) % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > len - done) chunk = len - done;

        struct vmo_page *p = page_get(o, index, false);
        if (!p) { rc = done ? EMBK_OK : -EMBK_ENOMEM; break; }

        memcpy(dst + done, (uint8_t *)page_va(p->phys) + within, (size_t)chunk);
        done += chunk;
    }

    mutex_unlock(&g_lock);
    *out_done = done;
    return rc;
}

int vmo_write(struct vm_object *o, uint64_t off, const void *buf, uint64_t len,
              uint64_t *out_done) {
    if (!o || (!buf && len) || !out_done)
        return -EMBK_EINVAL;
    *out_done = 0;

    mutex_lock(&g_lock);

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t done = 0;
    int rc = EMBK_OK;

    while (done < len) {
        uint64_t index = (off + done) / PAGE_SIZE;
        uint64_t within = (off + done) % PAGE_SIZE;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > len - done) chunk = len - done;

        /* A page written in FULL never needs reading first: every byte is
         * about to be replaced. A partial write does -- otherwise the bytes
         * the caller did not touch would come back as zeroes, which is a
         * read-modify-write bug that only shows on unaligned writes. The
         * exception is a page entirely past the old end of file: there is
         * nothing on the device to preserve, so zeroes are correct. */
        bool full = (within == 0 && chunk == PAGE_SIZE);
        bool past_eof = (index * PAGE_SIZE >= o->size);
        struct vmo_page *p = page_get(o, index, full || past_eof);
        if (!p) { rc = done ? EMBK_OK : -EMBK_ENOMEM; break; }

        memcpy((uint8_t *)page_va(p->phys) + within, src + done, (size_t)chunk);
        if (!p->dirty) {
            p->dirty = true;
            o->dirty_pages++;
            g_stats.dirty_pages++;
        }
        lru_touch(p);
        done += chunk;

        if (off + done > o->size)
            o->size = off + done;
    }

    mutex_unlock(&g_lock);
    *out_done = done;
    return rc;
}

int vmo_flush(struct vm_object *o) {
    if (!o)
        return -EMBK_EINVAL;
    mutex_lock(&g_lock);
    int rc = flush_object_locked(o);
    mutex_unlock(&g_lock);
    return rc;
}

uint64_t vmo_size(struct vm_object *o) {
    if (!o)
        return 0;
    mutex_lock(&g_lock);
    uint64_t n = o->size;
    mutex_unlock(&g_lock);
    return n;
}

/* Shared by both truncate entry points. Caller holds g_lock. */
static void truncate_locked(struct vm_object *o, uint64_t size) {
    o->size = size;
    uint64_t first_gone = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t b = 0; b < o->nbuckets; b++) {
        struct vmo_page *p = o->buckets[b];
        while (p) {
            struct vmo_page *next = p->hnext;
            if (p->index >= first_gone)
                page_destroy(o, p);
            p = next;
        }
    }
    /* The page STRADDLING the new end keeps its head and must lose its tail:
     * the file no longer has those bytes, and a later flush would write them
     * straight back. */
    if (size % PAGE_SIZE) {
        struct vmo_page *p = page_find(o, size / PAGE_SIZE);
        if (p && p->phys)
            memset((uint8_t *)page_va(p->phys) + (size % PAGE_SIZE), 0,
                   (size_t)(PAGE_SIZE - (size % PAGE_SIZE)));
    }
}

void vmo_truncated(struct vm_object *o, uint64_t size) {
    if (!o)
        return;
    mutex_lock(&g_lock);
    truncate_locked(o, size);
    mutex_unlock(&g_lock);
}

void vmo_file_truncated(struct vnode vn, uint64_t size) {
    if (!g_ready)
        return;
    mutex_lock(&g_lock);
    struct vm_object *o = registry_find(vn);
    if (o)
        truncate_locked(o, size);
    mutex_unlock(&g_lock);
}

void vmo_invalidate(struct vm_object *o) {
    if (!o)
        return;
    mutex_lock(&g_lock);
    o->no_writeback = true;
    obj_free_pages_locked(o);
    o->size = 0;
    mutex_unlock(&g_lock);
}

/* --- mapping a cached page ------------------------------------------------ */

uint64_t vmo_wire_page(struct vm_object *o, uint64_t index) {
    if (!o)
        return 0;
    mutex_lock(&g_lock);

    /* A mapping may legitimately reach PAST the end of the file -- mmap
     * rounds up to a page, and a file's last page is partly beyond it. Those
     * bytes must read as ZERO, so an absent page inside the mapping is filled
     * with zeroes rather than refused. */
    bool past_eof = (index * PAGE_SIZE >= o->size);
    struct vmo_page *p = page_get(o, index, past_eof || o->anon);  /* anonymous: zeroes, nothing to read */
    if (!p) { mutex_unlock(&g_lock); return 0; }

    p->wired++;
    /* A wired page is not evictable, so its place in the LRU is meaningless
     * until it is unwired -- but touching it keeps the ordering sane for when
     * that happens. */
    lru_touch(p);
    uint64_t phys = p->phys;
    mutex_unlock(&g_lock);
    return phys;
}

void vmo_unwire_page(struct vm_object *o, uint64_t index) {
    if (!o)
        return;
    mutex_lock(&g_lock);
    struct vmo_page *p = page_find(o, index);
    if (p && p->wired)
        p->wired--;
    mutex_unlock(&g_lock);
}

uint64_t vmo_page_phys(struct vm_object *o, uint64_t index) {
    if (!o)
        return 0;
    mutex_lock(&g_lock);
    struct vmo_page *p = page_find(o, index);
    uint64_t phys = p ? p->phys : 0;
    mutex_unlock(&g_lock);
    return phys;
}

void vmo_mark_dirty(struct vm_object *o, uint64_t index) {
    if (!o)
        return;
    mutex_lock(&g_lock);
    struct vmo_page *p = page_find(o, index);
    if (p && !p->dirty) {
        p->dirty = true;
        o->dirty_pages++;
        g_stats.dirty_pages++;
    }
    mutex_unlock(&g_lock);
}

/* --- anonymous objects ---------------------------------------------------- */

struct vm_object *vmo_create_anon(uint64_t size) {
    if (!g_ready || size == 0)
        return NULL;
    struct vm_object *o = kmalloc(sizeof *o);
    if (!o)
        return NULL;
    memset(o, 0, sizeof *o);
    o->buckets  = o->inline_buckets;
    o->nbuckets = VMO_HASH_BUCKETS;
    o->anon = true;
    o->no_writeback = true;          /* there is no file to write back TO */
    o->size = size;
    o->refs = 1;

    mutex_lock(&g_lock);
    o->reg_next = g_registry;
    g_registry = o;
    g_stats.anon_objects++;
    mutex_unlock(&g_lock);
    return o;
}

void vmo_set_mapper(struct vm_object *o, uint64_t pml4_phys, uint64_t base_va) {
    if (!o || !o->anon)
        return;
    mutex_lock(&g_lock);
    o->mapper_pml4 = pml4_phys;
    o->mapper_base = base_va;
    mutex_unlock(&g_lock);
}

/* Take a mapped page out of its mapper's page tables. Caller holds g_lock.
 * Nothing happens for a page that is not mapped: never touched since it came
 * back, or unmapped by a reclaim whose swap-out then failed. */
static void anon_unmap_locked(struct vm_object *o, struct vmo_page *p) {
    if (p->wired && o->mapper_pml4) {
        vmm_unmap_in(o->mapper_pml4, o->mapper_base + p->index * PAGE_SIZE);
        p->wired = 0;
    }
}

void vmo_discard_range(struct vm_object *o, uint64_t first, uint64_t count) {
    if (!o || !o->anon || count == 0)
        return;
    mutex_lock(&g_lock);
    for (uint32_t b = 0; b < o->nbuckets; b++) {
        struct vmo_page *p = o->buckets[b];
        while (p) {
            struct vmo_page *next = p->hnext;
            if (p->index >= first && p->index - first < count) {
                anon_unmap_locked(o, p);
                page_destroy(o, p);
            }
            p = next;
        }
    }
    mutex_unlock(&g_lock);
}

void vmo_detach_mapper(struct vm_object *o) {
    if (!o || !o->anon)
        return;
    mutex_lock(&g_lock);
    for (uint32_t b = 0; b < o->nbuckets; b++)
        for (struct vmo_page *p = o->buckets[b]; p; p = p->hnext)
            anon_unmap_locked(o, p);
    o->mapper_pml4 = 0;
    mutex_unlock(&g_lock);
}

uint64_t vmo_writeback_all(void) {
    if (!g_ready)
        return 0;
    mutex_lock(&g_lock);
    uint64_t before = g_stats.writebacks;
    for (struct vm_object *o = g_registry; o; o = o->reg_next)
        (void)flush_object_locked(o);
    uint64_t n = g_stats.writebacks - before;
    mutex_unlock(&g_lock);
    return n;
}

/* File pages, from the tail of the file LRU. Clean ones are dropped; dirty
 * ones are flushed first, and only when `allow_dirty`. Caller holds g_lock. */
static uint64_t reclaim_file_locked(uint64_t want, bool allow_dirty) {
    uint64_t freed = 0;
    struct vmo_page *p = g_lru_file.tail;
    while (p && freed < want) {
        struct vmo_page *prev = p->lru_prev;
        struct vm_object *o = p->owner;
        g_stats.lru_visited++;

        if (p->wired) { p = prev; continue; }

        if (p->dirty) {
            if (!allow_dirty) { p = prev; continue; }
            if (flush_run(o, p->index) == 0) { p = prev; continue; }
            if (p->dirty) { p = prev; continue; }
            prev = p->lru_prev;   /* flush_run may have moved neighbours */
        }

        page_destroy(o, p);
        g_stats.evictions++;
        freed++;
        p = prev;
    }
    return freed;
}

/* One cluster of unmapped anonymous pages goes to the store: as ONE device
 * command when a run of contiguous slots exists, one page at a time when it
 * does not. A page the store refuses stays resident and unmapped -- the next
 * touch maps it again, nothing is lost. Returns how many left RAM. */
static uint64_t anon_batch_out_locked(struct vmo_page **batch, int n) {
    uint64_t phys[SWAP_CLUSTER_PAGES], slots[SWAP_CLUSTER_PAGES];
    for (int i = 0; i < n; i++) phys[i] = batch[i]->phys;

    uint64_t ts = time_get_ns();
    if (swap_out_cluster(phys, n, slots) == 0)
        for (int i = 0; i < n; i++) slots[i] = swap_out(phys[i]);
    g_stats.swap_ns += time_get_ns() - ts;

    uint64_t out = 0;
    for (int i = 0; i < n; i++) {
        struct vmo_page *p = batch[i];
        if (!slots[i]) continue;
        struct vm_object *o = p->owner;
        lru_unlink(p);
        pmm_free_page(p->phys);
        p->phys = 0;
        p->swap_slot = slots[i];
        o->resident--;
        o->swapped++;
        g_stats.swapouts++;
        g_stats.evictions++;
        out++;
    }
    return out;
}

static bool g_second_chance = true;
void vmo_set_second_chance(bool on) { g_second_chance = on; }

/* Anonymous pages, from the tail of the anonymous LRU, a cluster at a time.
 * Caller holds g_lock and has already checked that the store can take them.
 *
 * SECOND CHANCE. The LRU only sees faults: a page is touched in it when it
 * comes in and never again, so its order is the order pages ARRIVED, and the
 * tail is the oldest arrival -- which under a stream of new pages is exactly
 * the hot set a program keeps coming back to. The hardware knows better: it
 * sets the accessed bit on every reference. So a candidate whose bit is set
 * is spared -- bit cleared, page moved to the head -- and the walk goes on;
 * it is evicted only if it is still unreferenced when the walk comes round
 * again. Two passes at most: the second takes pages regardless, so a call
 * always makes progress even when every page was referenced. This is CLOCK,
 * folded into the LRU walk the reclaimer already does. */
static uint64_t reclaim_anon_locked(uint64_t want) {
    uint64_t freed = 0;
    struct vmo_page *batch[SWAP_CLUSTER_PAGES];
    int n = 0;

  for (int pass = 0; pass < 2 && freed + (uint64_t)n < want; pass++) {
    struct vmo_page *p = g_lru_anon.tail;
    while (p && freed + (uint64_t)n < want) {
        struct vmo_page *prev = p->lru_prev;
        struct vm_object *o = p->owner;
        g_stats.lru_visited++;

        bool take = p->phys != 0;
        if (take && p->wired) {
            if (p->wired != 1 || !o->mapper_pml4) {
                take = false;
            } else {
                uint64_t va = o->mapper_base + p->index * PAGE_SIZE;
                if (pass == 0 && g_second_chance &&
                    vmm_test_and_clear_accessed_in(o->mapper_pml4, va)) {
                    g_stats.second_chances++;
                    lru_touch(p);              /* referenced: to the head, and on */
                    p = prev;
                    continue;
                }
                /* WIRED AND MAPPED, OR WIRED AND IN FLIGHT? The fault path
                 * wires a page BEFORE it maps it, and holds no lock of ours in
                 * between (it may have just waited on a disk to get here). A
                 * page wired but not yet in the page table is a fault in
                 * progress holding a frame it is about to map; swapping it out
                 * now would free that frame under it. The page table is the
                 * arbiter: present means mapped and evictable, absent means
                 * leave it for this round.
                 *
                 * OUT OF THE PAGE TABLE FIRST, then out to disk. The other
                 * order has a window in which the process stores through a
                 * PTE that still points at a frame whose contents are already
                 * on their way to the store, and that store is lost. Unmapping
                 * flushes every core's TLB (x86 broadcasts a shootdown, the
                 * aarch64 tlbi is broadcast by the hardware), so from this
                 * line a touch faults and waits on g_lock -- which is held
                 * until the page is safely in its slot. */
                if (vmm_get_phys_in(o->mapper_pml4, va) != p->phys) {
                    take = false;
                } else {
                    uint64_t tu = time_get_ns();
                    vmm_unmap_in(o->mapper_pml4, va);
                    g_stats.unmap_ns += time_get_ns() - tu;
                    p->wired = 0;
                }
            }
        }

        if (take) {
            batch[n++] = p;
            if (n == SWAP_CLUSTER_PAGES) {
                uint64_t out = anon_batch_out_locked(batch, n);
                freed += out;
                n = 0;
                if (out == 0)
                    return freed;       /* the store refused a whole cluster: full, or failing */
            }
        }
        p = prev;                       /* still valid: only g_lock holders touch the list */
    }
    /* Flush the partial batch BEFORE the next pass walks the list again: a
     * page still in the batch is still on the LRU, and the second pass would
     * collect it twice -- two slots written, the first one leaked. The audit
     * in `test swap` caught exactly that: 4 to 7 slots per run with no owner. */
    if (n) {
        freed += anon_batch_out_locked(batch, n);
        n = 0;
    }
  }
    return freed;
}

/* The reclaimer. Caller holds g_lock. `swap_ok` false confines it to file
 * pages -- what the cache-size cap wants, since a cache over its budget is not
 * memory pressure and should not cost a process its working set.
 *
 * THE ORDER IS THE POLICY, cheapest first: a clean file page costs a re-read,
 * a dirty one a writeback before it can even be considered, an anonymous page
 * a write to the swap store AND a read to bring it back. Each kind has its own
 * list, so a request that clean file pages can satisfy never looks at an
 * anonymous page. Anonymous pages are candidates only when there is somewhere
 * for them to go -- decided once, here, so a full or absent store does not
 * unmap a page and then find that out. */
static uint64_t reclaim_locked(uint64_t want, bool swap_ok) {
    uint64_t t_start = time_get_ns();
    g_stats.reclaim_calls++;

    uint64_t freed = reclaim_file_locked(want, false);
    if (freed < want)
        freed += reclaim_file_locked(want - freed, true);
    if (freed < want && swap_ok && swap_available() && swap_free_slots() > 0)
        freed += reclaim_anon_locked(want - freed);

    g_stats.reclaim_ns += time_get_ns() - t_start;
    return freed;
}

uint64_t vmo_reclaim(uint64_t want) {
    if (!g_ready || want == 0)
        return 0;
    mutex_lock(&g_lock);
    uint64_t freed = reclaim_locked(want, true);
    mutex_unlock(&g_lock);
    return freed;
}

uint64_t vmo_audit_swap(uint64_t *store_used, uint64_t *dangling, uint64_t *duplicates) {
    uint64_t out = 0, dang = 0, dup = 0;
    if (store_used) *store_used = 0;
    if (!g_ready) { if (dangling) *dangling = 0; if (duplicates) *duplicates = 0; return 0; }
    mutex_lock(&g_lock);
    /* A bit per slot seen so far, to catch two pages naming one slot. 32768
     * slots is 4 KiB; a store larger than 64 Mi slots is not audited for
     * duplicates (the bitmap is skipped), which is stated rather than hidden. */
    /* The store's own count, read HERE under the cache lock: no page-out or
     * page-in can happen between it and the walk, so the two are comparable.
     * (Read outside, they were not: a reclaim batch in progress moved the
     * page count by a hundred between two adjacent reads.) */
    struct swap_stats st; swap_stats_get(&st);
    if (store_used) *store_used = st.used;
    uint64_t nbits = st.nslots + 1;
    uint8_t *seen = (nbits <= (64ull << 20)) ? kmalloc((nbits + 7) / 8) : NULL;
    if (seen) memset(seen, 0, (nbits + 7) / 8);
    for (struct vm_object *o = g_registry; o; o = o->reg_next) {
        if (!o->anon) continue;
        for (uint32_t b = 0; b < o->nbuckets; b++)
            for (struct vmo_page *p = o->buckets[b]; p; p = p->hnext) {
                if (p->phys || !p->swap_slot) continue;
                out++;
                if (!swap_slot_held(p->swap_slot)) dang++;
                if (seen && p->swap_slot < nbits) {
                    if (seen[p->swap_slot >> 3] & (1u << (p->swap_slot & 7))) dup++;
                    else seen[p->swap_slot >> 3] |= (uint8_t)(1u << (p->swap_slot & 7));
                }
            }
    }
    mutex_unlock(&g_lock);
    if (seen) kfree(seen);
    if (dangling)   *dangling = dang;
    if (duplicates) *duplicates = dup;
    return out;
}

void vmo_stats_get(struct vmo_stats *out) {
    if (!out)
        return;
    if (!g_ready) { memset(out, 0, sizeof *out); return; }
    mutex_lock(&g_lock);
    *out = g_stats;
    out->resident_pages = 0;
    out->dirty_pages = 0;
    out->anon_pages = 0;
    out->swapped_pages = 0;
    for (struct vm_object *o = g_registry; o; o = o->reg_next) {
        if (o->anon) {
            out->anon_pages    += o->resident;
            out->swapped_pages += o->swapped;
            continue;
        }
        out->resident_pages += o->resident;
        out->dirty_pages += o->dirty_pages;
    }
    mutex_unlock(&g_lock);
}

/* ---- the writeback thread ----------------------------------------------- */

/* How long a dirty page may sit before it is pushed. Every write-back cache
 * is a bet that the machine will still be running in N seconds; this is N.
 * Short enough that an unexpected loss is bounded and describable, long
 * enough that a program appending a line at a time does not commit a
 * transaction per line -- which is exactly what the old write-through path
 * did, and the reason a `cp` of a large file was slower than the device. */
#define VMO_WRITEBACK_INTERVAL_MS 2000

/* Keep this many pages free at all times. When the allocator drops below it,
 * the cache gives pages back BEFORE anything has to fail an allocation -- the
 * point of a cache that consumes idle memory is that it stops being a cache
 * the instant the memory is wanted for something else. */
#define VMO_RECLAIM_LOW_PAGES   4096     /* 16 MiB */
#define VMO_RECLAIM_BATCH        512

/* How often the thread wakes to look at memory pressure. */
#define VMO_SCAN_INTERVAL_MS     100

static void vmo_writeback_main(void) {
    uint64_t next_flush = 0;
    while (1) {
        uint64_t now = timer_uptime_ms();
        if (now >= next_flush) {
            (void)vmo_writeback_all();
            next_flush = now + VMO_WRITEBACK_INTERVAL_MS;
        }

        /* Memory pressure is checked far more often than writeback runs: a
         * process asking for a large mapping should not have to wait out a
         * whole writeback interval for the cache to notice it is in the way. */
        if (pmm_free_pages() < VMO_RECLAIM_LOW_PAGES)
            (void)vmo_reclaim(VMO_RECLAIM_BATCH);
        else if (g_max_resident) {
            /* Over the CACHE budget, not under memory pressure: file pages
             * only. Anonymous memory is a process's working set, and a cache
             * that has grown large is no reason to page it out. */
            struct vmo_stats s;
            vmo_stats_get(&s);
            if (s.resident_pages > g_max_resident) {
                mutex_lock(&g_lock);
                (void)reclaim_locked(s.resident_pages - g_max_resident, false);
                mutex_unlock(&g_lock);
            }
        }

        /* A REAL sleep, not a yield loop. This thread wakes ten times a
         * second and does nothing the vast majority of those times; as a
         * yield loop it would have been permanently runnable and would have
         * kept a core out of idle for the life of the machine. */
        sched_sleep_ms(VMO_SCAN_INTERVAL_MS);
    }
}

void vmo_writeback_init(void) {
    mutex_init(&g_lock);

    /* Half of the memory that is free at boot. Not a tuning result -- a
     * deliberate ceiling, so the reclaimer is a safety net rather than the
     * only thing standing between the cache and the rest of the system. */
    g_max_resident = pmm_free_pages() / 2;

    g_ready = true;
    process_create_kthread(vmo_writeback_main, NULL);
    kprintf("pagecache: writeback thread started (interval %d ms, cap %u MiB)\n",
            VMO_WRITEBACK_INTERVAL_MS,
            (unsigned)(g_max_resident * PAGE_SIZE / (1024 * 1024)));
}
