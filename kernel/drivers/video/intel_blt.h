/* intel_blt.h -- the Gen9 blitter: the BCS command streamer, fed through an
 * execlist context, drawing through its own page tables.
 *
 * What it offers is small and exact: fill a rectangle of a surface with a
 * colour (XY_COLOR_BLT), copy a rectangle between surfaces (XY_SRC_COPY_BLT),
 * and know when the engine has done so (MI_FLUSH_DW writing a sequence
 * number to the status page). Surfaces are linear, 32 bits a pixel, and
 * addressed through an aliasing PPGTT this file builds: a range of graphics
 * addresses is given pages, cached (window pixels, the composed frame) or
 * uncached (a scanout the display reads straight from memory).
 *
 * How it reaches the hardware is two tables, so the host tests can run all
 * of it against a model of the engine (tests/drivers/intel_blt_model.h): the
 * registers (intel_regs.h) and the memory below. Bring-up ends with a probe
 * -- a fill and a copy, read back by the CPU -- and the engine is used only
 * if that came back right; otherwise it is reset and left alone, and the
 * display goes on without it. */
#ifndef EMBLINK_INTEL_BLT_H
#define EMBLINK_INTEL_BLT_H
#include <stdint.h>
#include "drivers/video/intel_regs.h"

struct intel_gpu_mem {
    uint64_t (*page_alloc)(void *ctx);                /* a zeroed 4 KiB page, by physical address; 0: none */
    void     (*page_free)(void *ctx, uint64_t phys);
    void    *(*page_kva)(void *ctx, uint64_t phys);    /* where the CPU reaches it */
    uint64_t (*va_phys)(void *ctx, const void *va);    /* a kernel address's page, 0 when unmapped */
    /* The GGTT: the engine fetches its ring, context and status page through it. */
    void     (*ggtt_set)(void *ctx, uint32_t index, uint64_t pte);
    uint64_t (*ggtt_get)(void *ctx, uint32_t index);
    /* GGTT room for the engine's own pages: `pages` 4 KiB pages, aligned to
     * 64 KiB; the graphics address, or 0 when there is none. Released with
     * its entries already cleared. */
    uint32_t (*ggtt_reserve)(void *ctx, uint32_t pages);
    void     (*ggtt_release)(void *ctx, uint32_t ggtt, uint32_t pages);
    void     *ctx;
};

/* Caching of a PPGTT mapping (an index into the private PAT this file sets). */
#define BLT_CACHED   0u        /* write-back, in the LLC: coherent with the CPU */
#define BLT_UNCACHED 3u        /* straight to memory: what the display engine reads */

/* PPGTT graphics addresses: a GGTT range mirrored at its own address lives
 * below BLT_VA_POOL; ranges this file hands out are above it. */
#define BLT_VA_POOL  0x40000000u
#define BLT_VA_END   0xffe00000u

struct intel_blt;

/* Bring the engine up and probe it. NULL (and the reason in the log) when it
 * could not be, in which case nothing was left running. */
struct intel_blt *intel_blt_init(const struct intel_regs *regs, const struct intel_gpu_mem *mem);
void intel_blt_fini(struct intel_blt *b);

/* Graphics addresses: `pages` pages from the pool (2 MiB aligned), and back. */
uint32_t intel_blt_va_alloc(struct intel_blt *b, uint32_t pages);
void     intel_blt_va_free(struct intel_blt *b, uint32_t va, uint32_t pages);
/* Point `n` pages at `va` at these physical pages; unmap them again. */
int  intel_blt_map(struct intel_blt *b, uint32_t va, const uint64_t *phys, uint32_t n, unsigned cache);
void intel_blt_unmap(struct intel_blt *b, uint32_t va, uint32_t n);
/* Mirror a GGTT range at the same graphics address (a scanout). */
int  intel_blt_mirror_ggtt(struct intel_blt *b, uint32_t ggtt, uint32_t pages, unsigned cache);

/* Commands, queued until intel_blt_submit. Rectangles are x, y, w, h in
 * pixels; addresses are of the surface's first pixel; pitch in bytes. */
int intel_blt_fill(struct intel_blt *b, uint64_t dst, uint32_t pitch, int32_t x, int32_t y, int32_t w, int32_t h,
                   uint32_t argb);
int intel_blt_copy(struct intel_blt *b, uint64_t dst, uint32_t dst_pitch, int32_t dx, int32_t dy,
                   uint64_t src, uint32_t src_pitch, int32_t sx, int32_t sy, int32_t w, int32_t h);
/* Send what is queued; returns its sequence number (0: nothing to send, or
 * the engine is gone). */
uint32_t intel_blt_submit(struct intel_blt *b);
/* Wait for a sequence number. 0, or -1 when the engine did not get there in
 * time: it is then reset and every later call fails. */
int intel_blt_wait(struct intel_blt *b, uint32_t seqno);
/* Submit and wait. */
int intel_blt_sync(struct intel_blt *b);
int intel_blt_dead(const struct intel_blt *b);

/* ---- what the engine and the tests agree on (Gen9, i915's names) ---- */
#define BCS_BASE              0x22000u
#define RING_TAIL(b)          ((b) + 0x30u)
#define RING_HEAD(b)          ((b) + 0x34u)
#define RING_START(b)         ((b) + 0x38u)
#define RING_CTL(b)           ((b) + 0x3cu)
#define RING_HWS_PGA(b)       ((b) + 0x80u)
#define RING_HWSTAM(b)        ((b) + 0x98u)
#define RING_MI_MODE(b)       ((b) + 0x9cu)
#define RING_IMR(b)           ((b) + 0xa8u)
#define RING_BBSTATE(b)       ((b) + 0x110u)
#define RING_SBBADDR(b)       ((b) + 0x114u)
#define RING_SBBSTATE(b)      ((b) + 0x118u)
#define RING_SBBADDR_UDW(b)   ((b) + 0x11cu)
#define RING_BBADDR(b)        ((b) + 0x140u)
#define RING_BBADDR_UDW(b)    ((b) + 0x168u)
#define RING_ELSP(b)          ((b) + 0x230u)
#define RING_CONTEXT_CONTROL(b) ((b) + 0x244u)
#define RING_PDP_LDW(b, n)    ((b) + 0x270u + (n) * 8u)
#define RING_PDP_UDW(b, n)    ((b) + 0x274u + (n) * 8u)
#define RING_MODE_GEN7(b)     ((b) + 0x29cu)
#define RING_CSB_PTR(b)       ((b) + 0x3a0u)
#define RING_CTX_TIMESTAMP(b) ((b) + 0x3a8u)
#define GEN6_GDRST            0x941cu
#define GEN6_GRDOM_BLT        (1u << 3)
#define GEN8_PRIVATE_PAT_LO   0x40e0u
#define GEN8_PRIVATE_PAT_HI   0x40e4u
/* i915's Gen9 private PAT: 0 WB in the LLC, 1 WC, 2 WT, 3 uncached, 4-7 WB by age. */
#define PPAT_LO_VALUE         0x000a0907u
#define PPAT_HI_VALUE         0x3b2b1b0bu
#define FORCEWAKE_RENDER_GEN9 0xa278u
#define FORCEWAKE_ACK_RENDER_GEN9 0x0d84u
#define FORCEWAKE_BLITTER_GEN9 0xa188u
#define FORCEWAKE_ACK_BLITTER_GEN9 0x130044u
#define GFX_FLSH_CNTL_GEN6    0x101008u

#define MASKED_ENABLE(bits)   (((uint32_t)(bits) << 16) | (bits))
#define MASKED_DISABLE(bits)  ((uint32_t)(bits) << 16)

/* The register state page of a Gen9 non-render context: dword index of
 * each LRI header and of each register's (offset, value) pair. */
#define CTX_LRI_HEADER_0      0x01u
#define CTX_CONTEXT_CONTROL   0x02u
#define CTX_RING_HEAD         0x04u
#define CTX_RING_TAIL         0x06u
#define CTX_RING_START        0x08u
#define CTX_RING_CTL          0x0au
#define CTX_BB_HEAD_U         0x0cu
#define CTX_BB_HEAD_L         0x0eu
#define CTX_BB_STATE          0x10u
#define CTX_SBB_HEAD_U        0x12u
#define CTX_SBB_HEAD_L        0x14u
#define CTX_SBB_STATE         0x16u
#define CTX_LRI_HEADER_1      0x21u
#define CTX_TIMESTAMP         0x22u
#define CTX_PDP3_UDW          0x24u           /* then PDP3_LDW, PDP2_UDW, ... PDP0_LDW at 0x32 */
#define CTX_PDP0_LDW          0x32u
#define CTX_CTRL_RESTORE_INHIBIT (1u << 0)
#define CTX_CTRL_INHIBIT_SYN     (1u << 3)
#define MI_LOAD_REGISTER_IMM(n) ((0x22u << 23) | (2u * (n) - 1u))
#define MI_NOOP               0u
#define MI_FLUSH_DW           (0x26u << 23)
#define MI_FLUSH_DW_STORE_DW  (1u << 14)
#define MI_INVALIDATE_TLB     (1u << 18)
#define MI_FLUSH_DW_USE_GTT   (1u << 2)
#define XY_COLOR_BLT          ((2u << 29) | (0x50u << 22))
#define XY_SRC_COPY_BLT       ((2u << 29) | (0x53u << 22))
#define BLT_WRITE_RGBA        (3u << 20)
#define BLT_DEPTH_32          (3u << 24)
#define ROP_PATCOPY           (0xf0u << 16)
#define ROP_SRCCOPY           (0xccu << 16)
#define CTX_DESC_VALID        (1ull << 0)
#define CTX_DESC_LEGACY_32B   (1ull << 3)     /* addressing mode 1: 32-bit, four PDPs */
#define CTX_DESC_PRIVILEGE    (1ull << 8)
#define GFX_RUN_LIST_ENABLE   (1u << 15)
#define GFX_REPLAY_MODE       (1u << 11)
#define STOP_RING             (1u << 8)
#define RING_VALID            1u
#define RING_BB_PPGTT         (1u << 5)
#define PTE_PRESENT           1ull
#define PTE_RW                2ull
#define PTE_PWT               (1ull << 3)
#define PTE_PCD               (1ull << 4)

/* Where things sit in the status page (dwords). */
#define HWS_CSB0              0x10u    /* the context status buffer, mirrored: 6 entries of 2 dwords */
#define HWS_CSB_WP            0x1fu    /* ... and its write pointer */
#define HWS_SEQNO             0x40u
#define HWS_SCRATCH           0x42u
#define CSB_ENTRIES           6u
#define CSB_IDLE_ACTIVE       (1u << 0)
#define CSB_ACTIVE_IDLE       (1u << 3)
#define CSB_COMPLETE          (1u << 4)

#endif
