/* intel_blt_model.h -- a Gen9 blitter command streamer, for the host tests.
 *
 * Physical memory, the GGTT and the registers of a Gen9 GT as intel_blt.c
 * meets them, and an engine that runs what is submitted to it the way the
 * hardware would -- and refuses whatever the hardware would not accept:
 *   - an ELSP write without both forcewake domains held, or before execlists
 *     are on, or a descriptor that is not a valid privileged 32-bit-legacy
 *     context;
 *   - a register state page not laid out as Gen9's (LRI headers, register
 *     offsets), a first run that does not inhibit the restore, a ring that is
 *     not valid or a tail that is not a qword;
 *   - any command but MI_NOOP, MI_FLUSH_DW, XY_COLOR_BLT and XY_SRC_COPY_BLT,
 *     or one of those with the wrong length, depth, ROP or write mask;
 *   - a PPGTT walk that finds no page (a GPU fault), and a write to a
 *     scanout page through anything but an uncached mapping.
 * Its TLB keeps translations until MI_FLUSH_DW invalidates them, so a driver
 * that remaps without invalidating draws from the old pages here as it would
 * on the machine. When the engine completes it writes the sequence number
 * where it was told, saves the context (HEAD) and reports idle in the
 * context status buffer mirrored in the status page. */
#ifndef EMBLINK_TEST_INTEL_BLT_MODEL_H
#define EMBLINK_TEST_INTEL_BLT_MODEL_H
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/video/intel_blt.h"

#define M_PHYS_BASE  0x200000000ull          /* above 4 GiB: PDPs and PTEs need their upper bits */
#define M_PHYS_BYTES (48u << 20)
#define M_PAGES      (M_PHYS_BYTES / 4096u)

static uint8_t m_mem[M_PHYS_BYTES] __attribute__((aligned(4096)));
static uint8_t m_used[M_PAGES], m_display[M_PAGES];
static uint32_t m_run[M_PAGES];               /* pages in an allocation, at its first page */
static uint64_t m_ggtt[1u << 20];
static uint32_t m_regs[0x200000 / 4];
static uint32_t m_ggtt_next = 0x10000000u;   /* the engine's GGTT room: above a 256 MiB aperture */
static uint64_t m_clock;
static unsigned m_resets, m_submits, m_hang, m_faults;
static uint32_t m_elsp[4];
static unsigned m_elsp_n;
static int m_ran_once;
struct m_tlbe { uint32_t vpn; uint64_t pte; int valid; };
static struct m_tlbe m_tlb[4096];

#define M_FAIL(...) do { printf("model: "); printf(__VA_ARGS__); puts(""); m_faults++; } while (0)

static void *m_ptr(uint64_t phys)
{
    if (phys < M_PHYS_BASE || phys >= M_PHYS_BASE + M_PHYS_BYTES) { M_FAIL("access outside memory at %llx", (unsigned long long)phys); return m_mem; }
    return m_mem + (phys - M_PHYS_BASE);
}

/* ---- memory for the driver ------------------------------------------------------ */
static uint64_t m_alloc_run(uint32_t n)
{
    for (uint32_t i = 0; i + n <= M_PAGES; i++) {
        uint32_t k = 0;
        while (k < n && !m_used[i + k]) k++;
        if (k == n) {
            for (k = 0; k < n; k++) m_used[i + k] = 1;
            m_run[i] = n;
            memset(m_mem + (uint64_t)i * 4096, 0, (uint64_t)n * 4096);
            return M_PHYS_BASE + (uint64_t)i * 4096;
        }
        i += k;
    }
    return 0;
}
static void m_free_run(uint64_t phys)
{
    uint32_t i = (uint32_t)((phys - M_PHYS_BASE) / 4096);
    for (uint32_t k = 0; k < m_run[i]; k++) { m_used[i + k] = 0; m_display[i + k] = 0; }
    m_run[i] = 0;
}
static uint64_t mm_page_alloc(void *c) { (void)c; return m_alloc_run(1); }
static void mm_page_free(void *c, uint64_t p) { (void)c; m_free_run(p); }
static void *mm_page_kva(void *c, uint64_t p) { (void)c; return m_ptr(p); }
static uint64_t mm_va_phys(void *c, const void *va)
{
    (void)c;
    const uint8_t *p = (const uint8_t *)va;
    if (p < m_mem || p >= m_mem + M_PHYS_BYTES) return 0;
    return M_PHYS_BASE + ((uint64_t)(p - m_mem) & ~4095ull);
}
static void mm_ggtt_set(void *c, uint32_t i, uint64_t pte) { (void)c; m_ggtt[i] = pte; }
static uint64_t mm_ggtt_get(void *c, uint32_t i) { (void)c; return m_ggtt[i]; }
static uint32_t mm_ggtt_reserve(void *c, uint32_t pages)
{
    (void)c;
    uint32_t at = m_ggtt_next;
    m_ggtt_next += (pages * 4096u + 0xffffu) & ~0xffffu;
    return at;
}
static void mm_ggtt_release(void *c, uint32_t ggtt, uint32_t pages)
{
    (void)c;
    for (uint32_t i = 0; i < pages; i++)
        if (m_ggtt[ggtt / 4096 + i]) M_FAIL("GGTT range released with entries still set");
}
static const struct intel_gpu_mem m_mem_ops = {
    mm_page_alloc, mm_page_free, mm_page_kva, mm_va_phys, mm_ggtt_set, mm_ggtt_get, mm_ggtt_reserve, mm_ggtt_release, 0,
};

/* ---- address translation -------------------------------------------------------- */
static int m_ggtt_tr(uint32_t addr, uint64_t *phys)
{
    uint64_t e = m_ggtt[addr >> 12];
    if (!(e & 1u)) { M_FAIL("GGTT fault at %x", addr); return -1; }
    *phys = (e & 0x7ffffff000ull) | (addr & 0xfffu);
    return 0;
}

static uint64_t m_pdp[4];

static void m_tlb_flush(void) { memset(m_tlb, 0, sizeof m_tlb); }

/* A PPGTT address: its page, through the TLB; *cache the PAT index. */
static int m_ppgtt_tr(uint64_t addr, uint64_t *phys, unsigned *cache)
{
    if (addr >> 32) { M_FAIL("PPGTT address beyond 4 GiB: %llx", (unsigned long long)addr); return -1; }
    uint32_t a = (uint32_t)addr, vpn = a >> 12;
    struct m_tlbe *t = &m_tlb[vpn % 4096];
    uint64_t e;
    if (t->valid && t->vpn == vpn) e = t->pte;
    else {
        uint64_t *pd = (uint64_t *)m_ptr(m_pdp[a >> 30] & ~4095ull);
        uint64_t pde = pd[(a >> 21) & 511u];
        if (!(pde & 1u)) { M_FAIL("PPGTT fault (no page table) at %x", a); return -1; }
        uint64_t *pt = (uint64_t *)m_ptr(pde & 0x7ffffff000ull);
        e = pt[(a >> 12) & 511u];
        if (!(e & 1u)) { M_FAIL("PPGTT fault (no page) at %x", a); return -1; }
        t->vpn = vpn; t->pte = e; t->valid = 1;
    }
    *phys = (e & 0x7ffffff000ull) | (a & 0xfffu);
    *cache = (unsigned)(((e >> 3) & 1u) | (((e >> 4) & 1u) << 1) | (((e >> 7) & 1u) << 2));
    return 0;
}

static uint32_t m_ppgtt_rd(uint64_t addr)
{
    uint64_t p; unsigned c;
    if (m_ppgtt_tr(addr, &p, &c)) return 0;
    return *(uint32_t *)m_ptr(p);
}

static void m_ppgtt_wr(uint64_t addr, uint32_t v)
{
    uint64_t p; unsigned c;
    if (m_ppgtt_tr(addr, &p, &c)) return;
    if (m_display[(p - M_PHYS_BASE) / 4096] && c != BLT_UNCACHED) {
        M_FAIL("scanout written through a cached mapping (PAT %u) at %llx", c, (unsigned long long)addr);
    }
    *(uint32_t *)m_ptr(p) = v;
}

/* ---- the engine ------------------------------------------------------------------ */
static uint32_t m_rd_ggtt32(uint32_t addr)
{
    uint64_t p;
    return m_ggtt_tr(addr, &p) ? 0 : *(uint32_t *)m_ptr(p);
}
static void m_wr_ggtt32(uint32_t addr, uint32_t v)
{
    uint64_t p;
    if (!m_ggtt_tr(addr, &p)) *(uint32_t *)m_ptr(p) = v;
}

static int m_blt_rect(uint32_t d2, uint32_t d3, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    *x = (int32_t)(d2 & 0xffff); *y = (int32_t)(d2 >> 16);
    *w = (int32_t)(d3 & 0xffff) - *x; *h = (int32_t)(d3 >> 16) - *y;
    return *w > 0 && *h > 0;
}

static void m_execute(uint32_t start, uint32_t size, uint32_t head, uint32_t tail)
{
    uint32_t at = head;
    while (at != tail) {
        uint32_t d[10];
        uint32_t cmd = m_rd_ggtt32(start + at);
        unsigned client = cmd >> 29, op = (cmd >> 22) & 0x7f, len;
        if (cmd == 0) len = 1;
        else if (client == 0 && ((cmd >> 23) & 0x3f) == 0x26) len = (cmd & 0x3f) + 2;
        else if (client == 2) len = (cmd & 0xff) + 2;
        else { M_FAIL("unknown command %08x", cmd); return; }
        if (len > 10) { M_FAIL("command %08x too long", cmd); return; }
        for (unsigned i = 0; i < len; i++) {
            if (at == tail) { M_FAIL("command %08x runs past the tail", cmd); return; }
            d[i] = m_rd_ggtt32(start + at);
            at = (at + 4) % size;
        }
        if (cmd == 0) continue;
        if (client == 0) {                                          /* MI_FLUSH_DW */
            if (len != 4) M_FAIL("MI_FLUSH_DW of %u dwords", len);
            if (cmd & MI_INVALIDATE_TLB) m_tlb_flush();
            if (((cmd >> 14) & 3u) == 1u) {
                if (!(d[1] & MI_FLUSH_DW_USE_GTT) || (d[1] & 7u) != MI_FLUSH_DW_USE_GTT || d[2])
                    M_FAIL("MI_FLUSH_DW post-sync to %08x:%08x", d[2], d[1]);
                m_wr_ggtt32(d[1] & ~7u, d[3]);
            }
            continue;
        }
        int32_t x, y, w, h;
        if (op == 0x50) {                                           /* XY_COLOR_BLT */
            if (len != 7 || (cmd & BLT_WRITE_RGBA) != BLT_WRITE_RGBA) { M_FAIL("XY_COLOR_BLT %08x", cmd); continue; }
            if ((d[1] & (3u << 24)) != BLT_DEPTH_32 || (d[1] & (0xffu << 16)) != ROP_PATCOPY) M_FAIL("XY_COLOR_BLT BR13 %08x", d[1]);
            int32_t pitch = (int16_t)(d[1] & 0xffff);
            uint64_t dst = d[4] | ((uint64_t)d[5] << 32);
            if (!m_blt_rect(d[2], d[3], &x, &y, &w, &h)) { M_FAIL("empty XY_COLOR_BLT"); continue; }
            for (int32_t j = 0; j < h; j++)
                for (int32_t i = 0; i < w; i++) m_ppgtt_wr(dst + (uint64_t)(y + j) * (uint32_t)pitch + (uint64_t)(x + i) * 4, d[6]);
        } else if (op == 0x53) {                                    /* XY_SRC_COPY_BLT */
            if (len != 10 || (cmd & BLT_WRITE_RGBA) != BLT_WRITE_RGBA) { M_FAIL("XY_SRC_COPY_BLT %08x", cmd); continue; }
            if ((d[1] & (3u << 24)) != BLT_DEPTH_32 || (d[1] & (0xffu << 16)) != ROP_SRCCOPY) M_FAIL("XY_SRC_COPY_BLT BR13 %08x", d[1]);
            int32_t dp = (int16_t)(d[1] & 0xffff), sp = (int16_t)(d[7] & 0xffff);
            uint64_t dst = d[4] | ((uint64_t)d[5] << 32), src = d[8] | ((uint64_t)d[9] << 32);
            int32_t sx = (int32_t)(d[6] & 0xffff), sy = (int32_t)(d[6] >> 16);
            if (!m_blt_rect(d[2], d[3], &x, &y, &w, &h)) { M_FAIL("empty XY_SRC_COPY_BLT"); continue; }
            for (int32_t j = 0; j < h; j++)
                for (int32_t i = 0; i < w; i++)
                    m_ppgtt_wr(dst + (uint64_t)(y + j) * (uint32_t)dp + (uint64_t)(x + i) * 4,
                               m_ppgtt_rd(src + (uint64_t)(sy + j) * (uint32_t)sp + (uint64_t)(sx + i) * 4));
        } else {
            M_FAIL("unknown blitter command %08x", cmd);
        }
    }
}

static void m_csb(uint32_t hws, uint32_t status)
{
    uint32_t wp = m_rd_ggtt32(hws + HWS_CSB_WP * 4);
    wp = (wp + 1) % CSB_ENTRIES;
    m_wr_ggtt32(hws + (HWS_CSB0 + 2 * wp) * 4, status);
    m_wr_ggtt32(hws + (HWS_CSB0 + 2 * wp + 1) * 4, 1);
    m_wr_ggtt32(hws + HWS_CSB_WP * 4, wp);
}

static void m_submit(uint64_t desc)
{
    m_submits++;
    if ((m_regs[FORCEWAKE_ACK_RENDER_GEN9 / 4] & 1u) != 1u || (m_regs[FORCEWAKE_ACK_BLITTER_GEN9 / 4] & 1u) != 1u)
        M_FAIL("ELSP written without forcewake");
    if (!(m_regs[RING_MODE_GEN7(BCS_BASE) / 4] & GFX_RUN_LIST_ENABLE)) M_FAIL("ELSP written with execlists off");
    if (!(desc & CTX_DESC_VALID) || ((desc >> 3) & 3u) != 1u || !(desc & CTX_DESC_PRIVILEGE))
        M_FAIL("context descriptor %016llx", (unsigned long long)desc);
    if (m_regs[GEN8_PRIVATE_PAT_LO / 4] != PPAT_LO_VALUE || m_regs[GEN8_PRIVATE_PAT_HI / 4] != PPAT_HI_VALUE)
        M_FAIL("private PAT not set");
    uint32_t lrca = (uint32_t)desc & 0xfffff000u, hws = m_regs[RING_HWS_PGA(BCS_BASE) / 4];
    uint64_t sp;
    if (m_ggtt_tr(lrca + 4096, &sp)) return;
    uint32_t *s = (uint32_t *)m_ptr(sp);
    static const uint32_t regs0[11] = { 0x244, 0x34, 0x30, 0x38, 0x3c, 0x168, 0x140, 0x110, 0x11c, 0x114, 0x118 };
    if (s[CTX_LRI_HEADER_0] != MI_LOAD_REGISTER_IMM(11)) M_FAIL("LRI header 0 is %08x", s[CTX_LRI_HEADER_0]);
    for (unsigned i = 0; i < 11; i++)
        if (s[CTX_CONTEXT_CONTROL + 2 * i] != BCS_BASE + regs0[i]) M_FAIL("context register %u at %x", i, s[CTX_CONTEXT_CONTROL + 2 * i]);
    if (s[CTX_LRI_HEADER_1] != MI_LOAD_REGISTER_IMM(9)) M_FAIL("LRI header 1 is %08x", s[CTX_LRI_HEADER_1]);
    if (s[CTX_TIMESTAMP] != RING_CTX_TIMESTAMP(BCS_BASE)) M_FAIL("timestamp register");
    for (unsigned n = 0; n < 4; n++) {
        unsigned atn = CTX_PDP3_UDW + 4 * (3 - n);
        if (s[atn] != RING_PDP_UDW(BCS_BASE, n) || s[atn + 2] != RING_PDP_LDW(BCS_BASE, n)) M_FAIL("PDP%u registers", n);
        m_pdp[n] = ((uint64_t)s[atn + 1] << 32) | s[atn + 3];
    }
    uint32_t cc = s[CTX_CONTEXT_CONTROL + 1];
    if (!m_ran_once && (cc & MASKED_ENABLE(CTX_CTRL_RESTORE_INHIBIT)) != MASKED_ENABLE(CTX_CTRL_RESTORE_INHIBIT))
        M_FAIL("first run without restore inhibit: %08x", cc);
    m_ran_once = 1;
    uint32_t ctl = s[CTX_RING_CTL + 1], start = s[CTX_RING_START + 1], head = s[CTX_RING_HEAD + 1], tail = s[CTX_RING_TAIL + 1];
    uint32_t size = (ctl & 0x1ff000u) + 4096u;
    if (!(ctl & RING_VALID)) M_FAIL("ring not valid");
    if ((tail & 7u) || tail >= size || head >= size) M_FAIL("head %x tail %x of a %x ring", head, tail, size);
    if (s[CTX_BB_STATE + 1] != RING_BB_PPGTT) M_FAIL("batch state %x", s[CTX_BB_STATE + 1]);
    if (m_hang) return;                                    /* takes the work, never finishes it */
    m_csb(hws, CSB_IDLE_ACTIVE);
    m_execute(start, size, head, tail);
    s[CTX_RING_HEAD + 1] = tail;                           /* the context save */
    m_csb(hws, CSB_COMPLETE | CSB_ACTIVE_IDLE);
}

static uint32_t mr_read(void *c, uint32_t off)
{
    (void)c;
    if (off == GEN6_GDRST) return 0;                       /* resets complete at once */
    return m_regs[off / 4];
}

static int m_masked(uint32_t off)
{
    return off == FORCEWAKE_RENDER_GEN9 || off == FORCEWAKE_BLITTER_GEN9 || off == RING_MODE_GEN7(BCS_BASE) ||
           off == RING_MI_MODE(BCS_BASE) || off == RING_CSB_PTR(BCS_BASE);
}

static void mr_write(void *c, uint32_t off, uint32_t v)
{
    (void)c;
    if (m_masked(off)) v = (m_regs[off / 4] & ~(v >> 16)) | (v & (v >> 16) & 0xffffu);
    m_regs[off / 4] = v;
    if (off == FORCEWAKE_RENDER_GEN9) m_regs[FORCEWAKE_ACK_RENDER_GEN9 / 4] = v & 1u;
    if (off == FORCEWAKE_BLITTER_GEN9) m_regs[FORCEWAKE_ACK_BLITTER_GEN9 / 4] = v & 1u;
    if (off == GEN6_GDRST && (v & GEN6_GRDOM_BLT)) {
        m_resets++;
        m_regs[RING_MODE_GEN7(BCS_BASE) / 4] = 0;
        m_elsp_n = 0;
        m_ran_once = 0;
        m_tlb_flush();
    }
    if (off == RING_ELSP(BCS_BASE)) {
        m_elsp[m_elsp_n++] = v;
        if (m_elsp_n == 4) {
            m_elsp_n = 0;
            if (m_elsp[0] || m_elsp[1]) M_FAIL("a second execlist element");
            m_submit(((uint64_t)m_elsp[2] << 32) | m_elsp[3]);
        }
    }
}

static uint64_t mr_now(void *c) { (void)c; return m_clock += 7; }

static const struct intel_regs m_regs_ops = { mr_read, mr_write, mr_now, 0 };

/* Memory the CPU and the engine share: pages of the model, contiguous. */
static void *m_alloc(uint64_t bytes)
{
    uint64_t p = m_alloc_run((uint32_t)((bytes + 4095) / 4096));
    return p ? m_ptr(p) : 0;
}
static void m_free(void *p)
{
    m_free_run(mm_va_phys(0, p));
}

#endif
