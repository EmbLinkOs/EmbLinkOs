#include "mm/swap.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "block/block.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/spinlock.h"

/* See swap.h. One store; a second device with the header is reported and
 * left alone, because a page whose slot number is ambiguous is a page that
 * comes back as the wrong page. */

static struct embk_block_device *g_dev;
static uint64_t  g_nslots;
static uint32_t  g_spp;                  /* device sectors per page */
static uint8_t  *g_bitmap;               /* one bit per slot; bit 0 (header) set */
static uint64_t  g_used;
static uint64_t  g_next;                 /* rotating first-fit cursor */
static spinlock_t g_lock = SPINLOCK_INIT;
static struct swap_stats g_st;

/* The cluster staging buffer: contiguous, kernel-range and below 4 GiB, so
 * buffer_dma_ok() says yes and the block layer hands the device ONE command
 * for the whole run instead of bouncing 32 KiB at a time through its own
 * buffer. 64 KiB of .bss. One reclaimer at a time uses it -- the page cache's
 * lock serialises reclaim -- and the flag turns a second concurrent user into
 * a fallback rather than a corruption. */
static uint8_t g_cluster_buf[SWAP_CLUSTER_PAGES * 4096] __attribute__((aligned(4096)));
static bool    g_cluster_busy;

static inline bool bm_test(uint64_t s) { return (g_bitmap[s >> 3] >> (s & 7)) & 1; }
static inline void bm_set(uint64_t s)  { g_bitmap[s >> 3] |= (uint8_t)(1u << (s & 7)); }
static inline void bm_clr(uint64_t s)  { g_bitmap[s >> 3] &= (uint8_t)~(1u << (s & 7)); }

uint64_t swap_init(void) {
    if (g_dev) return g_nslots ? g_nslots - 1 : 0;

    for (uint32_t i = 0; i < BLOCK_MAX_DEVICES; i++) {
        struct embk_block_device *d = embk_block_get(i);
        if (!d || !d->read || !d->write || d->block_size == 0 || d->block_size > 4096) continue;
        if ((4096 % d->block_size) != 0) continue;

        struct swap_header *h = kmalloc(4096);
        if (!h) return 0;
        uint32_t spp = 4096 / d->block_size;
        if (d->block_count < spp || embk_block_read(d, 0, spp, h) != EMBK_OK) { kfree(h); continue; }
        if (memcmp(h->magic, SWAP_MAGIC, 8) != 0) { kfree(h); continue; }
        if (h->version != SWAP_HEADER_VER || h->page_size != 4096) {
            kprintf("swap: %s: header version %u / page %u not understood; ignoring\n",
                    d->name, h->version, h->page_size);
            kfree(h); continue;
        }
        uint64_t dev_slots = d->block_count / spp;
        uint64_t nslots = h->nslots < dev_slots ? h->nslots : dev_slots;
        kfree(h);

        if (g_dev) {
            kprintf("swap: %s also carries a swap header; only the first store (%s) is used\n",
                    d->name, g_dev->name);
            continue;
        }
        g_bitmap = kmalloc((nslots + 7) / 8);
        if (!g_bitmap) return 0;
        memset(g_bitmap, 0, (nslots + 7) / 8);
        bm_set(0);                        /* the header is not a slot */
        g_dev = d; g_nslots = nslots; g_spp = spp; g_used = 1; g_next = 1;
        memset(&g_st, 0, sizeof g_st);
        strncpy(g_st.dev, d->name, sizeof g_st.dev - 1);
        kprintf("swap: %s: %llu slots (%llu MiB), page 4096\n", d->name,
                (unsigned long long)(nslots - 1), (unsigned long long)((nslots - 1) >> 8));
    }
    if (!g_dev) kprintf("swap: no store found (no block device carries an EMBKSWAP header)\n");
    return g_nslots ? g_nslots - 1 : 0;
}

bool swap_available(void) { return g_dev != NULL; }

uint64_t swap_out(uint64_t phys) {
    if (!g_dev || !phys) return 0;

    spin_lock(&g_lock);
    uint64_t slot = 0;
    for (uint64_t n = 0; n < g_nslots; n++) {
        uint64_t s = g_next + n; if (s >= g_nslots) s -= g_nslots;
        if (s == 0) continue;
        if (!bm_test(s)) { slot = s; bm_set(s); g_used++; g_next = s + 1; break; }
    }
    spin_unlock(&g_lock);
    if (!slot) return 0;

    /* The write happens OUTSIDE the lock: it is device I/O and may sleep. The
     * slot is already ours, so nothing else can be handed it meanwhile. */
    /* THROUGH THE BLOCK LAYER, NOT THE DRIVER. The ATA driver is marked
     * needs_kernel_range: it recovers a buffer's physical address with KV2P,
     * which is only right for kernel-image addresses. A page frame reached
     * through the direct map (P2V) is not one, and the first version of this
     * called dev->write() directly with exactly that -- the write "succeeded"
     * from a garbage DMA address and the read back failed with EIO. The
     * wrapper bounces through a kernel-range buffer when the driver needs it,
     * which costs a 4 KiB copy per page and one serialising lock; a swap-aware
     * DMA path is a later refinement, and a correct slow path beats a fast
     * wrong one. */
    int rc = embk_block_write(g_dev, slot * g_spp, g_spp, (const void *)P2V(phys));
    if (rc != EMBK_OK) {
        kprintf("swap: %s: write of slot %llu failed: %s -- the page stays in RAM\n",
                g_dev->name, (unsigned long long)slot, embk_strerror(rc));
        spin_lock(&g_lock); bm_clr(slot); g_used--; spin_unlock(&g_lock);
        return 0;
    }
    spin_lock(&g_lock); g_st.outs++; g_st.bytes_written += 4096; spin_unlock(&g_lock);
    return slot;
}

/* A run of n free slots, from the cursor onward and then from the start.
 * Caller holds g_lock. Marks the run used and returns its first slot, or 0. */
static uint64_t alloc_run_locked(int n) {
    for (int sweep = 0; sweep < 2; sweep++) {
        uint64_t s   = sweep == 0 ? g_next : 1;
        uint64_t end = sweep == 0 ? g_nslots : g_next;
        if (s == 0) s = 1;
        uint64_t run = 0, start = 0;
        for (; s < end; s++) {
            if (bm_test(s)) { run = 0; continue; }
            if (run == 0) start = s;
            if (++run == (uint64_t)n) {
                for (uint64_t k = 0; k < (uint64_t)n; k++) bm_set(start + k);
                g_used += (uint64_t)n;
                g_next  = start + (uint64_t)n;
                return start;
            }
        }
    }
    return 0;
}

int swap_out_cluster(const uint64_t *phys, int n, uint64_t *slots_out) {
    if (!g_dev || !phys || !slots_out || n <= 0 || n > SWAP_CLUSTER_PAGES) return 0;

    spin_lock(&g_lock);
    uint64_t first = g_cluster_busy ? 0 : alloc_run_locked(n);
    if (first) g_cluster_busy = true;
    spin_unlock(&g_lock);
    if (!first) return 0;

    for (int i = 0; i < n; i++)
        memcpy(g_cluster_buf + (size_t)i * 4096, (const void *)P2V(phys[i]), 4096);
    int rc = embk_block_write(g_dev, first * g_spp, (uint32_t)n * g_spp, g_cluster_buf);

    spin_lock(&g_lock);
    g_cluster_busy = false;
    if (rc != EMBK_OK) {
        for (int i = 0; i < n; i++) bm_clr(first + (uint64_t)i);
        g_used -= (uint64_t)n;
        spin_unlock(&g_lock);
        kprintf("swap: %s: cluster write at slot %llu failed: %s -- the pages stay in RAM\n",
                g_dev->name, (unsigned long long)first, embk_strerror(rc));
        return 0;
    }
    g_st.outs += (uint64_t)n;
    g_st.bytes_written += (uint64_t)n * 4096;
    g_st.clusters++;
    g_st.cluster_pages += (uint64_t)n;
    spin_unlock(&g_lock);

    for (int i = 0; i < n; i++) slots_out[i] = first + (uint64_t)i;
    return n;
}

int swap_in(uint64_t slot, uint64_t phys) {
    if (!g_dev || !phys) return -EMBK_ENODEV;
    if (slot == 0 || slot >= g_nslots) return -EMBK_EINVAL;
    spin_lock(&g_lock);
    bool held = bm_test(slot);
    spin_unlock(&g_lock);
    if (!held) return -EMBK_EINVAL;   /* reading a slot nobody owns is reading garbage */

    int rc = embk_block_read(g_dev, slot * g_spp, g_spp, (void *)P2V(phys));
    if (rc != EMBK_OK) return rc;
    spin_lock(&g_lock); g_st.ins++; g_st.bytes_read += 4096; spin_unlock(&g_lock);
    return EMBK_OK;
}

bool swap_slot_held(uint64_t slot) {
    if (!g_dev || slot == 0 || slot >= g_nslots) return false;
    spin_lock(&g_lock);
    bool held = bm_test(slot);
    spin_unlock(&g_lock);
    return held;
}

void swap_free(uint64_t slot) {
    if (!g_dev || slot == 0 || slot >= g_nslots) return;
    spin_lock(&g_lock);
    if (bm_test(slot)) { bm_clr(slot); g_used--; g_st.frees++; }
    spin_unlock(&g_lock);
}

uint64_t swap_free_slots(void) {
    if (!g_dev) return 0;
    spin_lock(&g_lock);
    uint64_t n = g_nslots - g_used;      /* both count slot 0 */
    spin_unlock(&g_lock);
    return n;
}

void swap_stats_get(struct swap_stats *out) {
    if (!out) return;
    spin_lock(&g_lock);
    *out = g_st;
    out->nslots = g_nslots ? g_nslots - 1 : 0;
    out->used   = g_used ? g_used - 1 : 0;
    spin_unlock(&g_lock);
}
