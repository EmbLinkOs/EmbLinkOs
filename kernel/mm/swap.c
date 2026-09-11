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

void swap_free(uint64_t slot) {
    if (!g_dev || slot == 0 || slot >= g_nslots) return;
    spin_lock(&g_lock);
    if (bm_test(slot)) { bm_clr(slot); g_used--; g_st.frees++; }
    spin_unlock(&g_lock);
}

void swap_stats_get(struct swap_stats *out) {
    if (!out) return;
    spin_lock(&g_lock);
    *out = g_st;
    out->nslots = g_nslots ? g_nslots - 1 : 0;
    out->used   = g_used ? g_used - 1 : 0;
    spin_unlock(&g_lock);
}
