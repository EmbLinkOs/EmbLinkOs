#include "mm/vm_object.h"
#include "mm/pmm.h"
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

/* Global LRU across every object: newest at the head. Reclaim walks from the
 * TAIL, which is the least recently used page in the system regardless of
 * which file it belongs to. Per-object LRUs would let a file that is read once
 * and never again hold pages a hot file needs. */
static struct vmo_page *g_lru_head = NULL;
static struct vmo_page *g_lru_tail = NULL;

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

static void lru_unlink(struct vmo_page *p) {
    if (p->lru_prev) p->lru_prev->lru_next = p->lru_next;
    else if (g_lru_head == p) g_lru_head = p->lru_next;
    if (p->lru_next) p->lru_next->lru_prev = p->lru_prev;
    else if (g_lru_tail == p) g_lru_tail = p->lru_prev;
    p->lru_prev = p->lru_next = NULL;
}

static void lru_touch(struct vmo_page *p) {
    if (g_lru_head == p)
        return;
    lru_unlink(p);
    p->lru_next = g_lru_head;
    if (g_lru_head) g_lru_head->lru_prev = p;
    g_lru_head = p;
    if (!g_lru_tail) g_lru_tail = p;
}

/* ---- pages within an object --------------------------------------------- */

static struct vmo_page *page_find(struct vm_object *o, uint64_t index) {
    for (struct vmo_page *p = o->buckets[index % VMO_HASH_BUCKETS]; p; p = p->hnext)
        if (p->index == index)
            return p;
    return NULL;
}

static void page_unlink(struct vm_object *o, struct vmo_page *p) {
    struct vmo_page **pp = &o->buckets[p->index % VMO_HASH_BUCKETS];
    while (*pp && *pp != p)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = p->hnext;
}

/* Release one page: its frame goes back to the allocator and the record with
 * it. The caller must have flushed it if it was dirty and the data mattered. */
static void page_destroy(struct vm_object *o, struct vmo_page *p) {
    lru_unlink(p);
    page_unlink(o, p);
    if (p->dirty && o->dirty_pages) o->dirty_pages--;
    o->resident--;
    pmm_free_page(p->phys);
    kfree(p);
}

/* Fetch page `index` from the filesystem into a fresh frame.
 *
 * A short read is not an error: it is the end of the file, and the rest of the
 * page must be ZERO rather than whatever the frame held. A page served with a
 * previous owner's bytes in its tail is a disclosure, and it is one that only
 * shows up on files whose length is not a multiple of the page size -- which
 * is almost all of them. */
static struct vmo_page *page_fill(struct vm_object *o, uint64_t index, bool zero_only) {
    uint64_t phys = pmm_alloc_page();
    if (!phys)
        return NULL;

    uint8_t *va = (uint8_t *)page_va(phys);
    memset(va, 0, PAGE_SIZE);

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

    struct vmo_page *p = kmalloc(sizeof *p);
    if (!p) {
        pmm_free_page(phys);
        return NULL;
    }
    p->index = index;
    p->phys  = phys;
    p->dirty = false;
    p->wired = 0;
    p->owner = o;
    p->lru_prev = p->lru_next = NULL;
    p->hnext = o->buckets[index % VMO_HASH_BUCKETS];
    o->buckets[index % VMO_HASH_BUCKETS] = p;
    o->resident++;
    lru_touch(p);
    return p;
}

/* The page for `index`, faulting it in if absent. */
static struct vmo_page *page_get(struct vm_object *o, uint64_t index, bool zero_only) {
    struct vmo_page *p = page_find(o, index);
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
        if (!p || !p->dirty)
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
    for (int b = 0; b < VMO_HASH_BUCKETS; b++) {
        struct vmo_page *p = o->buckets[b];
        while (p) {
            struct vmo_page *next = p->hnext;
            lru_unlink(p);
            pmm_free_page(p->phys);
            kfree(p);
            p = next;
        }
        o->buckets[b] = NULL;
    }
    o->resident = 0;
    o->dirty_pages = 0;
}

static struct vm_object *registry_find(struct vnode vn) {
    for (struct vm_object *o = g_registry; o; o = o->reg_next)
        if (o->vn.mnt == vn.mnt && o->vn.ino == vn.ino)
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
        o->refs++;
        mutex_unlock(&g_lock);
        return o;
    }

    o = kmalloc(sizeof *o);
    if (!o) {
        mutex_unlock(&g_lock);
        return NULL;
    }
    memset(o, 0, sizeof *o);
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
    if (--o->refs > 0) {
        mutex_unlock(&g_lock);
        return;
    }

    /* Last descriptor gone. Flush before dropping: a dirty page whose object
     * no one can name again is data that was accepted and then discarded. */
    (void)flush_object_locked(o);
    obj_free_pages_locked(o);
    registry_remove(o);
    if (g_stats.objects) g_stats.objects--;
    mutex_unlock(&g_lock);
    kfree(o);
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
    for (int b = 0; b < VMO_HASH_BUCKETS; b++) {
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
        if (p)
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
    struct vmo_page *p = page_get(o, index, past_eof);
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

uint64_t vmo_reclaim(uint64_t want) {
    if (!g_ready || want == 0)
        return 0;

    mutex_lock(&g_lock);
    uint64_t freed = 0;

    /* Two passes, and the order is the policy: CLEAN pages first, because
     * dropping one costs nothing but the re-read, while dropping a dirty one
     * costs a disk write before it can even be considered. Only if clean pages
     * do not cover the request is anything written back to make room. */
    for (int pass = 0; pass < 2 && freed < want; pass++) {
        struct vmo_page *p = g_lru_tail;
        while (p && freed < want) {
            struct vmo_page *prev = p->lru_prev;
            struct vm_object *o = p->owner;

            if (p->wired) { p = prev; continue; }

            if (p->dirty) {
                if (pass == 0) { p = prev; continue; }
                if (flush_run(o, p->index) == 0) { p = prev; continue; }
                if (p->dirty) { p = prev; continue; }
                prev = p->lru_prev;   /* flush_run may have moved neighbours */
            }

            page_destroy(o, p);
            g_stats.evictions++;
            freed++;
            p = prev;
        }
    }

    mutex_unlock(&g_lock);
    return freed;
}

void vmo_stats_get(struct vmo_stats *out) {
    if (!out)
        return;
    if (!g_ready) { memset(out, 0, sizeof *out); return; }
    mutex_lock(&g_lock);
    *out = g_stats;
    out->resident_pages = 0;
    out->dirty_pages = 0;
    for (struct vm_object *o = g_registry; o; o = o->reg_next) {
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
            struct vmo_stats s;
            vmo_stats_get(&s);
            if (s.resident_pages > g_max_resident)
                (void)vmo_reclaim(s.resident_pages - g_max_resident);
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
