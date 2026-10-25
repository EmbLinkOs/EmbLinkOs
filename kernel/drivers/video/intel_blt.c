/* intel_blt.c -- the Gen9 blitter engine. See intel_blt.h.
 *
 * The engine is driven the way Linux's i915 drives Gen9's engines: execlists.
 * One logical ring context belongs to this file -- its register state page
 * laid out as i915's gen8_xcs_offsets, a 16 KiB ring and an aliasing 32-bit
 * PPGTT behind its four PDPs -- and work goes in by writing the context's
 * ring tail and its descriptor to the execlist submit port. The engine says
 * how far it got through MI_FLUSH_DW post-sync writes to the status page, and
 * that it went idle through the context status buffer mirrored there.
 *
 * One submission is in flight at a time: the next waits for the context to
 * have gone idle, because the engine saves the context image on the way out
 * and writing it meanwhile would lose the new tail. Forcewake is held while
 * the engine is busy and dropped when it is idle, so the GT can sleep between
 * frames. */
#include "drivers/video/intel_blt.h"
#include "include/kstring.h"
#include "include/kprintf.h"

#define PAGE         4096u
#define RING_PAGES   4u
#define RING_BYTES   (RING_PAGES * PAGE)
#define CTX_PAGES    4u                  /* PPHWSP, register state, the engine's save area */
#define CHUNK        (2u << 20)          /* one page table's reach */
#define CHUNKS       ((BLT_VA_END - BLT_VA_POOL) / CHUNK)
#define TABLES       2048u               /* 4 GiB of 2 MiB */
#define WAIT_US      250000u
#define FW_WAIT_US   50000u
#define RING_SLACK   64u                 /* kept free for the closing flushes */
#define CTX_ID       1u

struct intel_blt {
    const struct intel_regs *r;
    const struct intel_gpu_mem *m;
    int dead, live;
    uint64_t hws_page, ring_page[RING_PAGES], ctx_page[CTX_PAGES], pd[4];
    uint32_t hws_ggtt, ring_ggtt, ctx_ggtt;
    volatile uint32_t *hws;
    uint32_t *state;                     /* the context's register state page */
    uint32_t head, tail;                 /* bytes into the ring */
    uint32_t seqno, inflight;            /* last emitted; the one the engine is running (0: idle) */
    uint32_t csb_read;
    int ran_once, tlb_dirty, fw_held;
    unsigned queued;                     /* dwords since the last submission */
    uint64_t pt[TABLES];                 /* each 2 MiB's page table, by physical address */
    uint8_t  pool[(CHUNKS + 7) / 8];
};

static struct intel_blt g_blt;

static uint32_t rd(const struct intel_blt *b, uint32_t off) { return intel_rd(b->r, off); }
static void wr(const struct intel_blt *b, uint32_t off, uint32_t v) { intel_wr(b->r, off, v); }
static uint64_t now(const struct intel_blt *b) { return b->r->now_us(b->r->ctx); }

/* ---- forcewake: the render and blitter (GT) domains --------------------------- */
static const uint32_t fw_req[2] = { FORCEWAKE_RENDER_GEN9, FORCEWAKE_BLITTER_GEN9 };
static const uint32_t fw_ack[2] = { FORCEWAKE_ACK_RENDER_GEN9, FORCEWAKE_ACK_BLITTER_GEN9 };

static int fw_wait(struct intel_blt *b, uint32_t ack, uint32_t want)
{
    uint64_t end = now(b) + FW_WAIT_US;
    while ((rd(b, ack) & 1u) != want)
        if (now(b) > end) return -1;
    return 0;
}

static int fw_get(struct intel_blt *b)
{
    if (b->fw_held) return 0;
    for (unsigned i = 0; i < 2; i++) {
        if (fw_wait(b, fw_ack[i], 0)) return -1;              /* the last release has landed */
        wr(b, fw_req[i], MASKED_ENABLE(1u));
        if (fw_wait(b, fw_ack[i], 1)) return -1;
    }
    b->fw_held = 1;
    return 0;
}

static void fw_put(struct intel_blt *b)
{
    if (!b->fw_held) return;
    for (unsigned i = 2; i-- > 0;) wr(b, fw_req[i], MASKED_DISABLE(1u));
    b->fw_held = 0;
}

/* The blitter domain reset; forcewake held by the caller. */
static void reset_engine(struct intel_blt *b)
{
    wr(b, GEN6_GDRST, GEN6_GRDOM_BLT);
    uint64_t end = now(b) + FW_WAIT_US;
    while ((rd(b, GEN6_GDRST) & GEN6_GRDOM_BLT) && now(b) < end) { }
}

/* Give up on the engine: reset it so nothing it was doing goes on. */
static void die(struct intel_blt *b, const char *why)
{
    if (b->dead) return;
    kprintf("intel-blt: %s; the blitter is reset and left alone\n", why);
    if (!fw_get(b)) reset_engine(b);
    fw_put(b);
    b->dead = 1;
    b->inflight = 0;
}

/* ---- pages and the GGTT --------------------------------------------------------- */
static uint64_t page(struct intel_blt *b)
{
    return b->m->page_alloc(b->m->ctx);
}

static void *kva(struct intel_blt *b, uint64_t phys)
{
    return b->m->page_kva(b->m->ctx, phys);
}

static uint32_t ggtt_place(struct intel_blt *b, const uint64_t *pages, uint32_t n)
{
    uint32_t at = b->m->ggtt_reserve(b->m->ctx, n);
    if (!at) return 0;
    for (uint32_t i = 0; i < n; i++) b->m->ggtt_set(b->m->ctx, at / PAGE + i, (pages[i] & 0x7ffffff000ull) | 1u);
    wr(b, GFX_FLSH_CNTL_GEN6, 1u);
    (void)rd(b, GFX_FLSH_CNTL_GEN6);
    return at;
}

static void ggtt_drop(struct intel_blt *b, uint32_t *at, uint32_t n)
{
    if (!*at) return;
    for (uint32_t i = 0; i < n; i++) b->m->ggtt_set(b->m->ctx, *at / PAGE + i, 0);
    wr(b, GFX_FLSH_CNTL_GEN6, 1u);
    b->m->ggtt_release(b->m->ctx, *at, n);
    *at = 0;
}

/* ---- the PPGTT: four page directories, page tables made as needed ----------- */
static uint64_t pte(uint64_t phys, unsigned cache)
{
    uint64_t v = (phys & 0x7ffffff000ull) | PTE_PRESENT | PTE_RW;
    if (cache & 1u) v |= PTE_PWT;
    if (cache & 2u) v |= PTE_PCD;
    return v;
}

static uint64_t *table(struct intel_blt *b, uint32_t va, int make)
{
    uint32_t chunk = va >> 21;
    if (!b->pt[chunk]) {
        if (!make) return 0;
        uint64_t p = page(b);
        if (!p) return 0;
        b->pt[chunk] = p;
        uint64_t *pd = (uint64_t *)kva(b, b->pd[va >> 30]);
        pd[(va >> 21) & 511u] = (p & 0x7ffffff000ull) | PTE_PRESENT | PTE_RW;
    }
    return (uint64_t *)kva(b, b->pt[chunk]);
}

int intel_blt_map(struct intel_blt *b, uint32_t va, const uint64_t *phys, uint32_t n, unsigned cache)
{
    if (b->dead || (va & (PAGE - 1)) || (uint64_t)va + (uint64_t)n * PAGE > 0x100000000ull) return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = va + i * PAGE;
        uint64_t *pt = table(b, a, 1);
        if (!pt) return -1;
        pt[(a >> 12) & 511u] = pte(phys[i], cache);
    }
    b->tlb_dirty = 1;
    return 0;
}

void intel_blt_unmap(struct intel_blt *b, uint32_t va, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = va + i * PAGE;
        uint64_t *pt = table(b, a, 0);
        if (pt) pt[(a >> 12) & 511u] = 0;
    }
    b->tlb_dirty = 1;
}

int intel_blt_mirror_ggtt(struct intel_blt *b, uint32_t ggtt, uint32_t pages, unsigned cache)
{
    if ((ggtt & (PAGE - 1)) || (uint64_t)ggtt + (uint64_t)pages * PAGE > BLT_VA_POOL) return -1;
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t e = b->m->ggtt_get(b->m->ctx, ggtt / PAGE + i);
        if (!(e & 1u)) return -1;
        uint64_t p = e & 0x7ffffff000ull;
        if (intel_blt_map(b, ggtt + i * PAGE, &p, 1, cache)) return -1;
    }
    return 0;
}

uint32_t intel_blt_va_alloc(struct intel_blt *b, uint32_t pages)
{
    uint32_t need = (pages + 511u) / 512u, run = 0;
    if (!need || b->dead) return 0;
    for (uint32_t c = 0; c < CHUNKS; c++) {
        if (b->pool[c / 8] & (1u << (c % 8))) { run = 0; continue; }
        if (++run == need) {
            uint32_t first = c + 1 - need;
            for (uint32_t k = first; k <= c; k++) b->pool[k / 8] |= (uint8_t)(1u << (k % 8));
            return BLT_VA_POOL + first * CHUNK;
        }
    }
    return 0;
}

void intel_blt_va_free(struct intel_blt *b, uint32_t va, uint32_t pages)
{
    if (va < BLT_VA_POOL) return;
    uint32_t first = (va - BLT_VA_POOL) / CHUNK, need = (pages + 511u) / 512u;
    intel_blt_unmap(b, va, pages);
    for (uint32_t k = first; k < first + need && k < CHUNKS; k++) b->pool[k / 8] &= (uint8_t)~(1u << (k % 8));
}

/* ---- the ring ------------------------------------------------------------------ */
static void ring_put(struct intel_blt *b, uint32_t dword)
{
    uint32_t *p = (uint32_t *)kva(b, b->ring_page[b->tail / PAGE]);
    p[(b->tail % PAGE) / 4] = dword;
    b->tail = (b->tail + 4) % RING_BYTES;
    b->queued++;
}

static uint32_t ring_used(const struct intel_blt *b)
{
    return (b->tail + RING_BYTES - b->head) % RING_BYTES;
}

/* Room for `n` dwords that must not straddle the ring's end -- a short end
 * is padded with MI_NOOP -- with RING_SLACK left for the closing flushes.
 * Everything after head is free: it is where the engine stopped. */
static int ring_room(struct intel_blt *b, uint32_t n)
{
    uint32_t bytes = n * 4, pad = b->tail + bytes > RING_BYTES ? RING_BYTES - b->tail : 0;
    if (ring_used(b) + pad + bytes + RING_SLACK > RING_BYTES) return -1;
    while (pad) { ring_put(b, MI_NOOP); pad -= 4; }
    return 0;
}

static void flush_dw(struct intel_blt *b, uint32_t extra, uint32_t index, uint32_t value)
{
    ring_put(b, MI_FLUSH_DW | MI_FLUSH_DW_STORE_DW | extra | 2u);
    ring_put(b, (b->hws_ggtt + index * 4u) | MI_FLUSH_DW_USE_GTT);
    ring_put(b, 0);
    ring_put(b, value);
}

/* Room for a command of `n` dwords, after the TLB invalidation a changed
 * mapping asks for -- submitting what is queued, and waiting, if need be. */
static int need(struct intel_blt *b, uint32_t n)
{
    if (b->dead) return -1;
    uint32_t total = n + (b->tlb_dirty ? 4u : 0u);
    if (ring_room(b, total) && (intel_blt_sync(b) || ring_room(b, total))) return -1;   /* empty after a sync */
    if (b->tlb_dirty) {
        flush_dw(b, MI_INVALIDATE_TLB, HWS_SCRATCH, 0);
        b->tlb_dirty = 0;
    }
    return 0;
}

/* ---- commands ------------------------------------------------------------------ */
static int fits16(int32_t x, int32_t y, int32_t w, int32_t h)
{
    return x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= 32767 && y + h <= 32767;
}

int intel_blt_fill(struct intel_blt *b, uint64_t dst, uint32_t pitch, int32_t x, int32_t y, int32_t w, int32_t h,
                   uint32_t argb)
{
    if (!fits16(x, y, w, h) || pitch > 32767u || (pitch & 3u) || (dst & 3u) || need(b, 8)) return -1;
    ring_put(b, XY_COLOR_BLT | BLT_WRITE_RGBA | (7u - 2u));
    ring_put(b, BLT_DEPTH_32 | ROP_PATCOPY | pitch);
    ring_put(b, ((uint32_t)y << 16) | (uint32_t)x);
    ring_put(b, ((uint32_t)(y + h) << 16) | (uint32_t)(x + w));
    ring_put(b, (uint32_t)dst);
    ring_put(b, (uint32_t)(dst >> 32));
    ring_put(b, argb);
    ring_put(b, MI_NOOP);                                   /* an even count keeps the tail a qword */
    return 0;
}

int intel_blt_copy(struct intel_blt *b, uint64_t dst, uint32_t dst_pitch, int32_t dx, int32_t dy,
                   uint64_t src, uint32_t src_pitch, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    if (!fits16(dx, dy, w, h) || !fits16(sx, sy, w, h) || dst_pitch > 32767u || src_pitch > 32767u ||
        ((dst_pitch | src_pitch) & 3u) || ((dst | src) & 3u) || need(b, 10)) return -1;
    ring_put(b, XY_SRC_COPY_BLT | BLT_WRITE_RGBA | (10u - 2u));
    ring_put(b, BLT_DEPTH_32 | ROP_SRCCOPY | dst_pitch);
    ring_put(b, ((uint32_t)dy << 16) | (uint32_t)dx);
    ring_put(b, ((uint32_t)(dy + h) << 16) | (uint32_t)(dx + w));
    ring_put(b, (uint32_t)dst);
    ring_put(b, (uint32_t)(dst >> 32));
    ring_put(b, ((uint32_t)sy << 16) | (uint32_t)sx);
    ring_put(b, src_pitch);
    ring_put(b, (uint32_t)src);
    ring_put(b, (uint32_t)(src >> 32));
    return 0;
}

/* ---- submission ------------------------------------------------------------------ */
static void write_context(struct intel_blt *b)
{
    uint32_t *s = b->state;
    /* The first run has no saved state to restore; later ones restore what
     * the engine saved when it went idle. */
    s[CTX_CONTEXT_CONTROL + 1] = b->ran_once
        ? MASKED_DISABLE(CTX_CTRL_RESTORE_INHIBIT) | MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN)
        : MASKED_ENABLE(CTX_CTRL_RESTORE_INHIBIT | CTX_CTRL_INHIBIT_SYN);
    s[CTX_RING_HEAD + 1] = b->head;
    s[CTX_RING_TAIL + 1] = b->tail;
    s[CTX_RING_START + 1] = b->ring_ggtt;
    s[CTX_RING_CTL + 1] = ((RING_BYTES - PAGE) & 0x1ff000u) | RING_VALID;
}

uint32_t intel_blt_submit(struct intel_blt *b)
{
    if (b->dead || !b->queued) return 0;
    if (b->inflight && intel_blt_wait(b, b->inflight)) return 0;
    /* The sequence number, after a flush of everything the blits wrote; the
     * ring's slack keeps room for it. */
    if (b->tail + 16u > RING_BYTES) while (b->tail) ring_put(b, MI_NOOP);
    if (!++b->seqno) b->seqno = 1;
    flush_dw(b, 0, HWS_SEQNO, b->seqno);
    write_context(b);
    if (fw_get(b)) { die(b, "forcewake would not come"); return 0; }
    uint64_t desc = CTX_DESC_VALID | CTX_DESC_LEGACY_32B | CTX_DESC_PRIVILEGE | b->ctx_ggtt | ((uint64_t)CTX_ID << 32);
    wr(b, RING_ELSP(BCS_BASE), 0);                       /* element 1: none */
    wr(b, RING_ELSP(BCS_BASE), 0);
    wr(b, RING_ELSP(BCS_BASE), (uint32_t)(desc >> 32));  /* element 0, upper then lower: the lower write submits */
    wr(b, RING_ELSP(BCS_BASE), (uint32_t)desc);
    b->ran_once = 1;
    b->inflight = b->seqno;
    b->queued = 0;
    return b->seqno;
}

int intel_blt_wait(struct intel_blt *b, uint32_t seqno)
{
    if (b->dead) return -1;
    if (!b->inflight || (int32_t)(seqno - b->inflight) < 0) return 0;   /* idle, or an older one: done */
    uint64_t end = now(b) + WAIT_US;
    while ((int32_t)(b->hws[HWS_SEQNO] - b->inflight) < 0)
        if (now(b) > end) { die(b, "the engine did not finish a submission in 250 ms"); return -1; }
    /* The context must be out -- saved -- before its image is written again. */
    for (int idle = 0; !idle;) {
        uint32_t wp = b->hws[HWS_CSB_WP];
        while (wp < CSB_ENTRIES && b->csb_read != wp) {
            b->csb_read = (b->csb_read + 1) % CSB_ENTRIES;
            if (b->hws[HWS_CSB0 + 2 * b->csb_read] & CSB_ACTIVE_IDLE) idle = 1;
        }
        if (!idle && now(b) > end) { die(b, "the context did not go idle"); return -1; }
    }
    b->head = b->tail;
    b->inflight = 0;
    fw_put(b);
    return 0;
}

int intel_blt_sync(struct intel_blt *b)
{
    if (b->dead) return -1;
    if (b->queued && !intel_blt_submit(b)) return -1;
    return b->inflight ? intel_blt_wait(b, b->inflight) : 0;
}

int intel_blt_dead(const struct intel_blt *b) { return !b || b->dead; }

/* ---- bring-up ------------------------------------------------------------------ */
static void build_context(struct intel_blt *b)
{
    uint32_t *s = b->state;
    memset(s, 0, PAGE);
    static const uint32_t ring_regs[11] = { 0x244, 0x34, 0x30, 0x38, 0x3c, 0x168, 0x140, 0x110, 0x11c, 0x114, 0x118 };
    s[CTX_LRI_HEADER_0] = MI_LOAD_REGISTER_IMM(11);
    for (unsigned i = 0; i < 11; i++) s[CTX_CONTEXT_CONTROL + 2 * i] = BCS_BASE + ring_regs[i];
    s[CTX_BB_STATE + 1] = RING_BB_PPGTT;
    s[CTX_LRI_HEADER_1] = MI_LOAD_REGISTER_IMM(9);
    s[CTX_TIMESTAMP] = RING_CTX_TIMESTAMP(BCS_BASE);
    for (unsigned n = 0; n < 4; n++) {                   /* PDP3 first, as the engine lists them */
        unsigned at = CTX_PDP3_UDW + 4 * (3 - n);
        s[at] = RING_PDP_UDW(BCS_BASE, n);
        s[at + 1] = (uint32_t)(b->pd[n] >> 32);
        s[at + 2] = RING_PDP_LDW(BCS_BASE, n);
        s[at + 3] = (uint32_t)b->pd[n];
    }
    write_context(b);
}

/* A fill and a copy into a scratch page, read back by the CPU. */
static int probe(struct intel_blt *b)
{
    uint64_t src = page(b), dst = page(b);
    uint32_t va = intel_blt_va_alloc(b, 2);
    int ok = 0;
    if (src && dst && va) {
        uint64_t both[2] = { src, dst };
        uint32_t *s = (uint32_t *)kva(b, src), *d = (uint32_t *)kva(b, dst);
        for (unsigned i = 0; i < 1024; i++) { s[i] = 0xff000000u | (i * 0x010203u); d[i] = 0; }
        if (!intel_blt_map(b, va, both, 2, BLT_CACHED) &&
            !intel_blt_fill(b, va + PAGE, 128, 0, 0, 32, 8, 0xff336699u) &&
            !intel_blt_copy(b, va + PAGE, 128, 4, 2, va, 128, 8, 1, 16, 4) &&
            !intel_blt_sync(b)) {
            ok = d[0] == 0xff336699u && d[31] == 0xff336699u && d[7 * 32 + 31] == 0xff336699u && d[8 * 32] == 0 &&
                 d[2 * 32 + 4] == s[1 * 32 + 8] && d[5 * 32 + 19] == s[4 * 32 + 23] &&
                 d[2 * 32 + 3] == 0xff336699u && d[2 * 32 + 20] == 0xff336699u;
        }
    }
    if (va) intel_blt_va_free(b, va, 2);
    if (src) b->m->page_free(b->m->ctx, src);
    if (dst) b->m->page_free(b->m->ctx, dst);
    return ok ? 0 : -1;
}

struct intel_blt *intel_blt_init(const struct intel_regs *regs, const struct intel_gpu_mem *mem)
{
    struct intel_blt *b = &g_blt;
    memset(b, 0, sizeof *b);
    b->r = regs; b->m = mem;
    int have = (b->hws_page = page(b)) != 0;
    for (unsigned i = 0; i < RING_PAGES; i++) have &= (b->ring_page[i] = page(b)) != 0;
    for (unsigned i = 0; i < CTX_PAGES; i++) have &= (b->ctx_page[i] = page(b)) != 0;
    for (unsigned i = 0; i < 4; i++) have &= (b->pd[i] = page(b)) != 0;
    if (!have) { kprintf("intel-blt: no memory for the engine's pages\n"); intel_blt_fini(b); return 0; }
    b->hws_ggtt = ggtt_place(b, &b->hws_page, 1);
    b->ring_ggtt = ggtt_place(b, b->ring_page, RING_PAGES);
    b->ctx_ggtt = ggtt_place(b, b->ctx_page, CTX_PAGES);
    if (!b->hws_ggtt || !b->ring_ggtt || !b->ctx_ggtt) {
        kprintf("intel-blt: no GGTT room for the ring, context and status page\n");
        intel_blt_fini(b);
        return 0;
    }
    b->hws = (volatile uint32_t *)kva(b, b->hws_page);
    b->state = (uint32_t *)kva(b, b->ctx_page[1]);

    if (fw_get(b)) { kprintf("intel-blt: forcewake would not come\n"); intel_blt_fini(b); return 0; }
    reset_engine(b);                                     /* a known state, whatever the firmware left */
    wr(b, GEN8_PRIVATE_PAT_LO, PPAT_LO_VALUE);
    wr(b, GEN8_PRIVATE_PAT_HI, PPAT_HI_VALUE);
    wr(b, RING_HWSTAM(BCS_BASE), 0xffffffffu);           /* no interrupt writes the status page */
    wr(b, RING_MODE_GEN7(BCS_BASE), MASKED_DISABLE(GFX_REPLAY_MODE) | MASKED_ENABLE(GFX_RUN_LIST_ENABLE));
    wr(b, RING_MI_MODE(BCS_BASE), MASKED_DISABLE(STOP_RING));
    wr(b, RING_HWS_PGA(BCS_BASE), b->hws_ggtt);
    (void)rd(b, RING_HWS_PGA(BCS_BASE));
    wr(b, RING_IMR(BCS_BASE), 0xffffffffu);              /* polled, not interrupted */
    /* After a reset the status buffer is written from entry 0: read from the
     * one before it. */
    wr(b, RING_CSB_PTR(BCS_BASE), (0x7u << 24) | ((CSB_ENTRIES - 1) << 8));
    b->csb_read = CSB_ENTRIES - 1;
    b->hws[HWS_CSB_WP] = CSB_ENTRIES - 1;
    fw_put(b);
    build_context(b);
    b->live = 1;
    if (probe(b)) {
        die(b, "its probe blit did not come back right");
        intel_blt_fini(b);
        return 0;
    }
    kprintf("intel-blt: the blitter is up (execlists, aliasing PPGTT), probe blit correct\n");
    return b;
}

void intel_blt_fini(struct intel_blt *b)
{
    if (!b) return;
    if (b->live && !b->dead) intel_blt_sync(b);
    ggtt_drop(b, &b->hws_ggtt, 1);
    ggtt_drop(b, &b->ring_ggtt, RING_PAGES);
    ggtt_drop(b, &b->ctx_ggtt, CTX_PAGES);
    for (uint32_t c = 0; c < TABLES; c++)
        if (b->pt[c]) { b->m->page_free(b->m->ctx, b->pt[c]); b->pt[c] = 0; }
    uint64_t *all[] = { &b->hws_page, &b->pd[0], &b->pd[1], &b->pd[2], &b->pd[3] };
    for (unsigned i = 0; i < 5; i++) if (*all[i]) { b->m->page_free(b->m->ctx, *all[i]); *all[i] = 0; }
    for (unsigned i = 0; i < RING_PAGES; i++) if (b->ring_page[i]) { b->m->page_free(b->m->ctx, b->ring_page[i]); b->ring_page[i] = 0; }
    for (unsigned i = 0; i < CTX_PAGES; i++) if (b->ctx_page[i]) { b->m->page_free(b->m->ctx, b->ctx_page[i]); b->ctx_page[i] = 0; }
    b->live = 0;
    b->dead = 1;
}
