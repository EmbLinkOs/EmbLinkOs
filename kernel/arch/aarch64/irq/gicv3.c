#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* See gicv3.h. Arm Generic Interrupt Controller Architecture Specification,
 * GIC architecture version 3 and 4. */

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
#define GICR_SGI_BASE      0x10000
#define GICR_IGROUPR0      (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0    (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0    (GICR_SGI_BASE + 0x0180)
#define GICR_ICPENDR0      (GICR_SGI_BASE + 0x0280)
#define GICR_ICACTIVER0    (GICR_SGI_BASE + 0x0380)
#define GICR_IPRIORITYR    (GICR_SGI_BASE + 0x0400)
#define GICR_ICFGR1        (GICR_SGI_BASE + 0x0C04)

#define SPURIOUS 1023
#define MAX_INTID 1020

/* Middle priority for everything. A single priority means no interrupt can
 * preempt another, which is what we want while the kernel runs with a single
 * IRQ stack: nesting needs a stack per level and there is exactly one.
 * NOTE the GIC only implements the HIGH bits of the priority field, so 0x80 is
 * chosen because it is representable on every implementation. */
#define IRQ_PRIORITY 0x80

static volatile uint8_t *gicd;
static volatile uint8_t *gicr;      /* this CPU's redistributor */
static bool gic_ready;

static struct {
    gic_handler_t fn;
    const char   *name;
    uint64_t      count;
} handlers[MAX_INTID];

static uint64_t spurious;

static inline uint32_t d_read(uint32_t off)            { return *(volatile uint32_t *)(gicd + off); }
static inline void     d_write(uint32_t off, uint32_t v){ *(volatile uint32_t *)(gicd + off) = v; }
static inline void     d_write64(uint32_t off, uint64_t v){ *(volatile uint64_t *)(gicd + off) = v; }
static inline uint32_t r_read(uint32_t off)            { return *(volatile uint32_t *)(gicr + off); }
static inline void     r_write(uint32_t off, uint32_t v){ *(volatile uint32_t *)(gicr + off) = v; }
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

int gic_init(void) {
    fdt_node_t node = fdt_find_compatible("arm,gic-v3");
    if (node == FDT_NONE) {
        kprintf("gic: no arm,gic-v3 node in the device tree.\n");

        /* Say WHAT is there instead, because the overwhelmingly likely cause
         * is a QEMU command line without gic-version=3 -- under TCG, `-M virt`
         * still defaults to GICv2 -- and "not found" alone sends you looking
         * in the wrong place entirely. */
        if (fdt_find_compatible("arm,cortex-a15-gic") != FDT_NONE ||
            fdt_find_compatible("arm,gic-400") != FDT_NONE)
            kprintf("gic: this machine has a GICv2. Add gic-version=3 to -M virt.\n");

        /* Deliberately no GICv2 fallback: docs/ARM64.md §6.2 chose v3, and a
         * half-configured controller is worse than none -- interrupts would
         * appear to be enabled and simply never arrive. */
        return -1;
    }

    uint64_t d_base = 0, d_size = 0, r_base = 0, r_size = 0;
    if (!fdt_reg(node, 0, &d_base, &d_size) ||
        !fdt_reg(node, 1, &r_base, &r_size)) {
        kprintf("gic: device tree node has no distributor/redistributor reg\n");
        return -1;
    }

    /* Through the MMIO window, not the physical address: the identity map was
     * dropped at the end of A2 and physical addresses no longer translate. */
    gicd = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + d_base);
    volatile uint8_t *rbase = (volatile uint8_t *)(uintptr_t)(MMIO_BASE + r_base);

    kprintf("gic: GICD %p (%d KiB), GICR %p (%d KiB) [from the device tree]\n",
            (void *)(uintptr_t)d_base, (int)(d_size / 1024),
            (void *)(uintptr_t)r_base, (int)(r_size / 1024));

    gicr = find_redistributor((uint64_t)(uintptr_t)rbase, r_size);
    if (!gicr) {
        kprintf("gic: no redistributor frame matches this CPU's MPIDR\n");
        return -1;
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

    /* --- this CPU's redistributor ----------------------------------------- */
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

    gic_ready = true;
    kprintf("gic: initialised (GICv3, system register CPU interface)\n");
    return 0;
}

void gic_enable(uint32_t intid) {
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

void gic_disable(uint32_t intid) {
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

int gic_register(uint32_t intid, gic_handler_t handler, const char *name) {
    if (intid >= MAX_INTID || !handler)
        return -1;

    handlers[intid].fn    = handler;
    handlers[intid].name  = name ? name : "?";
    handlers[intid].count = 0;
    gic_enable(intid);

    kprintf("gic: INTID %d -> %s\n", (int)intid, handlers[intid].name);
    return 0;
}

void gic_unregister(uint32_t intid) {
    if (intid >= MAX_INTID)
        return;
    gic_disable(intid);
    handlers[intid].fn = 0;
}

void gic_dispatch(void) {
    uint64_t iar = SYSREG_READ(ICC_IAR1_EL1);
    uint32_t intid = (uint32_t)(iar & 0xFFFFFF);

    if (intid >= MAX_INTID) {          /* 1020-1023: spurious or special */
        spurious++;
        return;                        /* no EOI for a spurious ID       */
    }

    /* END OF INTERRUPT FIRST, THEN THE HANDLER.
     *
     * Not the obvious order, and it is load-bearing: the timer handler
     * CONTEXT-SWITCHES, so it does not return. With EOI after the handler,
     * the interrupt would stay active until this thread was scheduled again,
     * the GIC would refuse to deliver another at the same priority in the
     * meantime, and the other thread would never get a tick -- one preemption
     * and then silence.
     *
     * Safe here because every interrupt runs at one priority (IRQ_PRIORITY)
     * with PSTATE.I masked for the whole exception, so nothing can nest in
     * between and there is no ordering requirement to violate. */
    SYSREG_WRITE(ICC_EOIR1_EL1, iar);

    handlers[intid].count++;
    if (handlers[intid].fn)
        handlers[intid].fn(intid);
    else
        kprintf("gic: unhandled INTID %d\n", (int)intid);
}

uint64_t gic_count(uint32_t intid) {
    return intid < MAX_INTID ? handlers[intid].count : 0;
}

uint64_t gic_spurious_count(void) { return spurious; }

void gic_dump(void) {
    kprintf("gic: registered lines:\n");
    for (uint32_t i = 0; i < MAX_INTID; i++)
        if (handlers[i].fn)
            kprintf("gic:   INTID %-4d %-16s %d deliveries\n",
                    (int)i, handlers[i].name, (int)handlers[i].count);
    kprintf("gic:   spurious: %d\n", (int)spurious);
}
