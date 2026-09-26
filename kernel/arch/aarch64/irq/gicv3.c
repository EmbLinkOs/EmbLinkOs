#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/irq/gic_hw.h"
#include "arch/aarch64/smp/smp.h"
#include "arch/aarch64/cpu/percpu.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* The GICv3 back-end -- see gic_hw.h for the split, gicv3.h for the API.
 * Arm Generic Interrupt Controller Architecture Specification, GIC
 * architecture version 3 and 4. */

/* --- distributor ---------------------------------------------------------- */
#define GICD_CTLR          0x0000
#define GICD_TYPER         0x0004
#define GICD_IIDR          0x0008
#define GICD_IGROUPR       0x0080   /* +4 per 32 INTIDs */
#define GICD_ISENABLER     0x0100
#define GICD_ICENABLER     0x0180
#define GICD_ICPENDR       0x0280
#define GICD_ICACTIVER     0x0380
#define GICD_IPRIORITYR    0x0400   /* +1 per INTID  */
#define GICD_ICFGR         0x0C00   /* 2 bits per INTID */
#define GICD_IROUTER       0x6000   /* 8 bytes per INTID, from 32 */

#define GICD_CTLR_ENGRP0   (1u << 0)
#define GICD_CTLR_ENGRP1   (1u << 1)
#define GICD_CTLR_ARE      (1u << 4)
#define GICD_CTLR_RWP      (1u << 31)

/* --- redistributor -------------------------------------------------------- */
#define GICR_CTLR          0x0000
#define GICR_TYPER         0x0008
#define GICR_WAKER         0x0014
#define GICR_WAKER_SLEEP   (1u << 1)   /* ProcessorSleep  */
#define GICR_WAKER_ASLEEP  (1u << 2)   /* ChildrenAsleep  */

/* The SGI/PPI registers live in the redistributor's SECOND 64 KiB frame. */
/* LPI control, in the redistributor's FIRST frame. LPIs are how an MSI
 * arrives: the ITS translates a device's write into an LPI INTID and targets a
 * redistributor, which needs a configuration table (one byte per LPI: priority
 * and an enable bit) and a pending table (one bit) to hold that state. Both
 * are in memory the GIC reads by DMA, not registers. */
#define GICR_CTLR_ENABLE_LPIS  (1u << 0)
#define GICR_PROPBASER     0x0070
#define GICR_PENDBASER     0x0078

#define GICR_SGI_BASE      0x10000
#define GICR_IGROUPR0      (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0    (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0    (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0      (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0    (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR    (GICR_SGI_BASE + 0x0400)
#define GICR_ICFGR1        (GICR_SGI_BASE + 0x0C04)

#define MAX_INTID GIC_MAX_INTID

/* LPIs live at 8192 and up, far outside the SPI/PPI range, so they cannot
 * share the handlers[] array -- indexing it by INTID would need 8192 wasted
 * entries before the first useful one. They get their own small table, indexed
 * by LPI NUMBER (INTID - 8192), and gic_dispatch routes to it.
 *
 * 64 is generous: one MSI per device on a machine with a handful of virtio
 * devices. The number is a table size, not a hardware limit. */
#define LPI_BASE_INTID  8192
#define LPI_MAX         64
#define LPI_IDBITS      14                 /* INTIDs up to 16383 */
#define LPI_PROP_BYTES  ((1u << LPI_IDBITS) - LPI_BASE_INTID)
#define LPI_PEND_BYTES  (64u * 1024u)      /* PENDBASER is 64 KiB aligned+sized */

/* The GIC reads both by DMA, so they must be where KV2P() works -- .bss, not
 * the heap. Same constraint every virtqueue in this tree records. */
static uint8_t lpi_prop[LPI_PROP_BYTES] __attribute__((aligned(4096)));
static uint8_t lpi_pend[LPI_PEND_BYTES] __attribute__((aligned(65536)));

static gic_handler_t lpi_handlers[LPI_MAX];
static const char   *lpi_names[LPI_MAX];
static uint64_t      lpi_counts[LPI_MAX];

#define IRQ_PRIORITY GIC_PRIORITY    /* one priority for everything: gic.c */

static volatile uint8_t *gicd;
static volatile uint8_t *gicr;      /* this CPU's redistributor */

/* The redistributor ARRAY, kept so a secondary can find its OWN frame. Each
 * core has one, selected by matching its MPIDR against GICR_TYPER -- which is
 * exactly what find_redistributor() already does, it just needs to be called
 * again on the core that is asking. */
static volatile uint8_t *gicr_array;
static uint64_t          gicr_array_size;
static bool gic_ready;

static inline uint32_t d_read(uint32_t off)            { return *(volatile uint32_t *)(gicd + off); }
static inline void     d_write(uint32_t off, uint32_t v){ *(volatile uint32_t *)(gicd + off) = v; }
static inline void     d_write64(uint32_t off, uint64_t v){ *(volatile uint64_t *)(gicd + off) = v; }
static inline uint32_t r_read(uint32_t off)            { return *(volatile uint32_t *)(gicr + off); }
static inline void     r_write(uint32_t off, uint32_t v){ *(volatile uint32_t *)(gicr + off) = v; }
static inline void     r_write64(uint32_t off, uint64_t v){ *(volatile uint64_t *)(gicr + off) = v; }
static inline void     d_write8(uint32_t off, uint8_t v){ *(volatile uint8_t *)(gicd + off) = v; }
static inline void     r_write8(uint32_t off, uint8_t v){ *(volatile uint8_t *)(gicr + off) = v; }

/* The CPU interface is system registers. These encodings are spelled out
 * rather than using the register names so the file assembles regardless of
 * which -march the compiler was invoked with -- the named forms need the GIC
 * system register extension enabled, and a build that silently loses it would
 * fail at assembly time in a way that reads like a typo. */
#define ICC_PMR_EL1      "S3_0_C4_C6_0"
#define ICC_IAR1_EL1     "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1    "S3_0_C12_C12_1"
#define ICC_BPR1_EL1     "S3_0_C12_C12_3"
#define ICC_CTLR_EL1     "S3_0_C12_C12_4"
#define ICC_SRE_EL1      "S3_0_C12_C12_5"
#define ICC_IGRPEN1_EL1  "S3_0_C12_C12_7"

#define SYSREG_READ(name)                                        \
    ({ uint64_t _v; __asm__ volatile("mrs %0, " name : "=r"(_v)); _v; })
#define SYSREG_WRITE(name, val)                                  \
    do { __asm__ volatile("msr " name ", %0" :: "r"((uint64_t)(val))); } while (0)

/* Writes to some distributor registers take effect asynchronously; RWP says
 * when. Not waiting is a classic source of "the interrupt I just disabled
 * arrived anyway". */
static void gicd_wait_rwp(void) {
    int spins = 1000000;
    while ((d_read(GICD_CTLR) & GICD_CTLR_RWP) && --spins)
        __asm__ volatile("dsb sy" ::: "memory");
    if (!spins)
        kprintf("gic: WARNING GICD_CTLR.RWP never cleared\n");
}

static void gicr_wait_rwp(void) {
    int spins = 1000000;
    while ((r_read(GICR_CTLR) & (1u << 3)) && --spins)
        __asm__ volatile("dsb sy" ::: "memory");
    if (!spins)
        kprintf("gic: WARNING GICR_CTLR.RWP never cleared\n");
}

/* Find this CPU's redistributor. They are a contiguous array of frames, each
 * tagged in GICR_TYPER with the affinity of the CPU it belongs to, terminated
 * by a frame with the Last bit set. Matching on MPIDR rather than assuming
 * "frame 0 is us" is what makes this correct when A9 starts other cores. */
static volatile uint8_t *find_redistributor(uint64_t base, uint64_t size) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

    /* Affinity as GICR_TYPER packs it: Aff3.Aff2.Aff1.Aff0 in bits 63:32. */
    uint64_t aff = ((mpidr >> 32) & 0xFFULL) << 24 |
                   ((mpidr >> 16) & 0xFFULL) << 16 |
                   ((mpidr >>  8) & 0xFFULL) << 8  |
                    (mpidr        & 0xFFULL);

    for (uint64_t off = 0; off + 0x20000 <= size; off += 0x20000) {
        volatile uint8_t *frame = (volatile uint8_t *)(uintptr_t)(base + off);
        uint64_t typer = *(volatile uint64_t *)(frame + GICR_TYPER);

        if (((typer >> 32) & 0xFFFFFFFFULL) == aff)
            return frame;
        if (typer & (1ULL << 4))       /* Last */
            break;
    }
    return 0;
}

static int v3_init_this_cpu(void);
static const struct gic_hw gicv3_hw;

int gicv3_probe(const struct gic_hw **out) {
    fdt_node_t node = fdt_find_compatible("arm,gic-v3");
    if (node == FDT_NONE)
        return -1;              /* not a GICv3 machine: gic.c tries v2 */

    uint64_t d_base = 0, d_size = 0, r_base = 0, r_size = 0;
    if (!fdt_reg(node, 0, &d_base, &d_size) ||
        !fdt_reg(node, 1, &r_base, &r_size)) {
        kprintf("gic: device tree node has no distributor/redistributor reg\n");
        return -2;
    }

    /* Through the MMIO window, not the physical address: the identity map was
     * dropped at the end of A2 and physical addresses no longer translate. */
    gicd = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + d_base);
    volatile uint8_t *rbase = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + r_base);

    kprintf("gic: GICD %p (%d KiB), GICR %p (%d KiB) [from the device tree]\n",
            (void *)(uintptr_t)d_base, (int)(d_size / 1024),
            (void *)(uintptr_t)r_base, (int)(r_size / 1024));

    gicr_array      = rbase;
    gicr_array_size = r_size;

    gicr = find_redistributor((uint64_t)(uintptr_t)rbase, r_size);
    if (!gicr) {
        kprintf("gic: no redistributor frame matches this CPU's MPIDR\n");
        return -2;
    }

    uint32_t typer = d_read(GICD_TYPER);
    uint32_t lines = ((typer & 0x1F) + 1) * 32;
    if (lines > MAX_INTID)
        lines = MAX_INTID;
    kprintf("gic: %d interrupt lines, IIDR %p\n", (int)lines,
            (void *)(uintptr_t)d_read(GICD_IIDR));

    /* --- distributor ------------------------------------------------------ */
    d_write(GICD_CTLR, 0);
    gicd_wait_rwp();

    for (uint32_t i = 32; i < lines; i += 32) {
        d_write(GICD_ICENABLER + (i / 32) * 4, 0xFFFFFFFFu);
        d_write(GICD_ICPENDR   + (i / 32) * 4, 0xFFFFFFFFu);
        d_write(GICD_ICACTIVER + (i / 32) * 4, 0xFFFFFFFFu);
        d_write(GICD_IGROUPR   + (i / 32) * 4, 0xFFFFFFFFu);  /* Group 1 NS */
    }
    for (uint32_t i = 32; i < lines; i++) {
        d_write8(GICD_IPRIORITYR + i, IRQ_PRIORITY);
        /* Route every SPI to this CPU. Affinity routing (ARE) addresses CPUs
         * by MPIDR rather than by a bitmask, which is the other big GICv2
         * difference and the reason IROUTER is 64 bits wide. */
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        d_write64(GICD_IROUTER + i * 8, mpidr & 0xFF00FFFFFFULL);
    }
    gicd_wait_rwp();

    /* ARE must be set before Group enables, and both are needed: without ARE
     * the IROUTER writes above are ignored and SPIs go nowhere. */
    d_write(GICD_CTLR, GICD_CTLR_ARE | GICD_CTLR_ENGRP1 | GICD_CTLR_ENGRP0);
    gicd_wait_rwp();

    gic_ready = true;           /* before the per-core half: it enables lines */
    if (v3_init_this_cpu() != 0) {
        gic_ready = false;
        return -2;
    }

    *out = &gicv3_hw;
    return 0;
}

/* Everything in the controller that belongs to ONE CORE rather than to the
 * machine: waking its redistributor, configuring its 32 SGIs and PPIs, and
 * enabling its ICC_* system register CPU interface.
 *
 * Split out of gic_init() for SMP (A9). The distributor above is configured
 * once, by core 0; this half must be run BY each core, because every register
 * it touches is either in that core's own redistributor frame or is a banked
 * system register. A secondary that skips it has a GIC that looks initialised
 * from core 0's point of view and delivers it nothing. */
static int v3_init_this_cpu(void) {
    if (!gicr_array)
        return -1;

    /* Find THIS core's frame -- find_redistributor() reads the caller's own
     * MPIDR, so calling it again here is the whole of "per core". */
    gicr = find_redistributor((uint64_t)(uintptr_t)gicr_array, gicr_array_size);
    if (!gicr) {
        kprintf("gic: no redistributor frame matches this CPU's MPIDR\n");
        return -1;
    }

    uint32_t waker = r_read(GICR_WAKER);
    r_write(GICR_WAKER, waker & ~GICR_WAKER_SLEEP);
    {
        int spins = 1000000;
        while ((r_read(GICR_WAKER) & GICR_WAKER_ASLEEP) && --spins)
            __asm__ volatile("dsb sy" ::: "memory");
        if (!spins) {
            kprintf("gic: redistributor never woke (ChildrenAsleep stuck)\n");
            return -1;
        }
    }

    /* --- LPIs, which is what an MSI becomes -----------------------------
     * The configuration table says which LPIs are enabled and at what
     * priority; the pending table is the GIC's own scratch. Both are per
     * redistributor in principle -- and shared here, deliberately: every core
     * agrees about which LPIs exist and what they are worth, and one table is
     * one thing to keep consistent rather than N.
     *
     * PROPBASER's low bits carry ID_bits-1, not a size in bytes, and the
     * shareability/cacheability fields matter: a GIC told the table is
     * non-cacheable will read stale bytes through a coherent interconnect.
     * Inner-shareable, read-allocate write-back is what every other DMA
     * structure in this kernel uses. */
    if (!lpi_prop[0]) {
        /* Priority in bits[7:2], bit0 = enable. Every LPI starts DISABLED and
         * is turned on by gic_register_lpi(); a table of enabled interrupts
         * with no handlers is how a machine wedges on its first MSI. */
        for (uint32_t i = 0; i < LPI_PROP_BYTES; i++)
            lpi_prop[i] = IRQ_PRIORITY & 0xFC;
    }

    r_write64(GICR_PROPBASER,
              KV2P((uint64_t)(uintptr_t)lpi_prop)
              | (uint64_t)(LPI_IDBITS - 1)
              | (1ULL << 10)          /* InnerCache = RaWb */
              | (1ULL << 7));         /* Shareability = Inner Shareable */
    r_write64(GICR_PENDBASER,
              KV2P((uint64_t)(uintptr_t)lpi_pend)
              | (1ULL << 10) | (1ULL << 7));
    __asm__ volatile("dsb sy" ::: "memory");
    r_write(GICR_CTLR, r_read(GICR_CTLR) | GICR_CTLR_ENABLE_LPIS);
    __asm__ volatile("dsb sy; isb" ::: "memory");

    r_write(GICR_ICENABLER0, 0xFFFFFFFFu);   /* all SGIs+PPIs off to start */
    r_write(GICR_ICPENDR0,   0xFFFFFFFFu);
    r_write(GICR_ICACTIVER0, 0xFFFFFFFFu);
    r_write(GICR_IGROUPR0,   0xFFFFFFFFu);   /* Group 1 NS */
    for (uint32_t i = 0; i < 32; i++)
        r_write8(GICR_IPRIORITYR + i, IRQ_PRIORITY);
    gicr_wait_rwp();

    /* --- this CPU's system register interface ------------------------------ */
    /* SRE=1 selects system registers over the memory-mapped CPU interface. It
     * can be RAO/WI or RAZ/WI depending on the implementation, so it is read
     * back: if this bit will not stick there is no GICv3 CPU interface here
     * and every ICC_* access below would be undefined. */
    uint64_t sre = SYSREG_READ(ICC_SRE_EL1);
    SYSREG_WRITE(ICC_SRE_EL1, sre | 1);
    __asm__ volatile("isb" ::: "memory");
    if (!(SYSREG_READ(ICC_SRE_EL1) & 1)) {
        kprintf("gic: ICC_SRE_EL1.SRE will not set -- no system register interface\n");
        return -1;
    }

    SYSREG_WRITE(ICC_PMR_EL1, 0xFF);      /* accept every priority         */
    SYSREG_WRITE(ICC_BPR1_EL1, 0);        /* no preemption grouping        */
    SYSREG_WRITE(ICC_CTLR_EL1, 0);        /* EOImode 0: EOI both drops and
                                           * deactivates, which is what the
                                           * single-priority scheme above
                                           * wants                          */
    SYSREG_WRITE(ICC_IGRPEN1_EL1, 1);
    __asm__ volatile("isb" ::: "memory");

    return 0;
}

/* Claim an LPI: install a handler and ENABLE it in the configuration table.
 *
 * The table is memory the GIC caches, so a change to it is not visible until
 * the redistributor is told to re-read -- which is what the ITS's INV command
 * does. its.c issues that after calling this. */
int gic_register_lpi(uint32_t intid, gic_handler_t handler, const char *name) {
    if (intid < LPI_BASE_INTID || intid >= LPI_BASE_INTID + LPI_MAX)
        return -1;
    uint32_t l = intid - LPI_BASE_INTID;

    lpi_handlers[l] = handler;
    lpi_names[l]    = name;
    lpi_prop[intid - LPI_BASE_INTID] = (IRQ_PRIORITY & 0xFC) | 1u;   /* enable */
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

/* The PHYSICAL address of this core's redistributor -- what the ITS's MAPC
 * and SYNC commands target. The ITS addresses redistributors by address, not
 * by CPU number. */
uint64_t gic_redistributor_phys(void) {
    return (uint64_t)(uintptr_t)gicr - MMIO_BASE;
}

/* This core's GIC processor number -- GICR_TYPER[23:8]. The ITS names a
 * redistributor this way when GITS_TYPER.PTA is 0. It is NOT the CPU index and
 * NOT the MPIDR; it is the GIC's own numbering. */
/* Diagnostics for the ITS bring-up: did the redistributor accept LPIs, and did
 * a translated interrupt actually reach its pending table? Those two answers
 * separate "the ITS never translated it" from "it translated it and the CPU
 * interface never delivered it", which look identical from the handler. */
uint32_t gic_redist_ctlr(void) { return r_read(GICR_CTLR); }

int gic_lpi_pending(uint32_t intid) {
    if (intid < LPI_BASE_INTID) return -1;
    uint32_t l = intid - LPI_BASE_INTID + LPI_BASE_INTID;   /* absolute INTID */
    return (lpi_pend[l / 8] >> (l % 8)) & 1;
}

uint32_t gic_processor_number(void) {
    uint64_t typer = *(volatile uint64_t *)(gicr + GICR_TYPER);
    return (uint32_t)((typer >> 8) & 0xFFFF);
}

uint64_t gic_lpi_count(uint32_t intid) {
    if (intid < LPI_BASE_INTID || intid >= LPI_BASE_INTID + LPI_MAX)
        return 0;
    return lpi_counts[intid - LPI_BASE_INTID];
}

static void v3_enable(uint32_t intid) {
    if (!gic_ready || intid >= MAX_INTID)
        return;

    if (intid < 32) {
        r_write(GICR_ISENABLER0, 1u << intid);
        gicr_wait_rwp();
    } else {
        d_write(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
        gicd_wait_rwp();
    }
}

static void v3_disable(uint32_t intid) {
    if (!gic_ready || intid >= MAX_INTID)
        return;

    if (intid < 32) {
        r_write(GICR_ICENABLER0, 1u << intid);
        gicr_wait_rwp();
    } else {
        d_write(GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
        gicd_wait_rwp();
    }
}

/* --- acknowledge / end: the ICC_* system registers ----------------------- */

static uint64_t v3_ack(void)               { return SYSREG_READ(ICC_IAR1_EL1); }
static uint32_t v3_iar_intid(uint64_t iar) { return (uint32_t)(iar & 0xFFFFFF); }
static void     v3_eoi(uint64_t iar)       { SYSREG_WRITE(ICC_EOIR1_EL1, iar); }

void gicv3_lpi_dispatch(uint32_t intid) {
    uint32_t l = intid - LPI_BASE_INTID;
    if (l < LPI_MAX) {
        lpi_counts[l]++;
        if (lpi_handlers[l])
            lpi_handlers[l](intid);
    }
}

/* --- SGIs: ICC_SGI1R_EL1 ----------------------------------------------------
 * Sending one is a system-register write with no memory-mapped controller
 * involved at all, which is the same trade the rest of GICv3 makes. */

/* IRM = 1 means "every core except this one" -- the same shorthand x86's ICR
 * has, and for the same reason: it cannot miss a core that came up after a
 * list was built. Affinity fields are ignored with IRM set. */
#define SGI_IRM_ALL_BUT_SELF (1ULL << 40)
#define ICC_SGI1R_EL1        "S3_0_C12_C11_5"

static void sgi1r_write(uint64_t val) {
    __asm__ volatile("dsb ishst" ::: "memory");
    SYSREG_WRITE(ICC_SGI1R_EL1, val);
    __asm__ volatile("isb" ::: "memory");
}

static bool v3_sgi_all_but_self(uint32_t sgi) {
    sgi1r_write(SGI_IRM_ALL_BUT_SELF | ((uint64_t)sgi << 24));
    return true;
}

/* ONE core. IRM=0 means "use the affinity fields", and the target is a 16-bit
 * MASK of cores within one Aff1 cluster -- so this builds the mask from that
 * core's own MPIDR rather than from its index.
 *
 * Refusing a target outside this cluster rather than silently sending to the
 * wrong core: on `virt` every core shares Aff1..Aff3, so the case does not
 * arise here, and a machine where it does would otherwise wake a stranger. */
static bool v3_sgi_to_cpu(uint32_t cpu, uint32_t sgi) {
    uint64_t self = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
    uint64_t tgt = smp_cpu_mpidr(cpu);

    uint64_t aff1 = (tgt >> 8)  & 0xFF;
    uint64_t aff2 = (tgt >> 16) & 0xFF;
    uint64_t aff3 = (tgt >> 32) & 0xFF;
    if (aff1 != ((self >> 8) & 0xFF) || aff2 != ((self >> 16) & 0xFF) ||
        aff3 != ((self >> 32) & 0xFF))
        return false;                     /* another cluster: not ours to target */

    uint64_t aff0 = tgt & 0xFF;
    if (aff0 > 15)
        return false;                     /* outside this target list's 16 bits */

    sgi1r_write(((uint64_t)sgi << 24) |
                (aff3 << 48) | (aff2 << 32) | (aff1 << 16) |
                (1ULL << aff0));          /* IRM = 0: use the affinity fields */
    return true;
}

static const struct gic_hw gicv3_hw = {
    .name             = "GICv3, system register CPU interface",
    .init_this_cpu    = v3_init_this_cpu,
    .enable           = v3_enable,
    .disable          = v3_disable,
    .ack              = v3_ack,
    .iar_intid        = v3_iar_intid,
    .eoi              = v3_eoi,
    .sgi_all_but_self = v3_sgi_all_but_self,
    .sgi_to_cpu       = v3_sgi_to_cpu,
};
