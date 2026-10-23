#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/irq/gic_hw.h"
#include "arch/aarch64/cpu/percpu.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/vmm.h"
#include "include/kprintf.h"

/* The GICv2 back-end -- docs/RPI4.md P1. The Raspberry Pi 4's GIC-400 is one;
 * so is QEMU `virt` with gic-version=2, which is how this file gets the whole
 * `virt` suite as a witness and not only the Pi's boot.
 *
 * Arm Generic Interrupt Controller Architecture Specification, versions 1 and
 * 2 (IHI 0048B). Against the v3 in gicv3.c:
 *
 *   * the CPU interface is MMIO (GICC_*), not system registers. Every core
 *     uses the SAME address and the hardware banks it by which core asked;
 *   * there are no redistributors. The per-core state -- INTIDs 0-31, the
 *     SGIs and PPIs -- is banked inside the distributor, at the same offsets
 *     as everything else, which is why enable/disable need no special case;
 *   * a core is named by its CPU-INTERFACE NUMBER, a bit in an 8-bit mask,
 *     not by its MPIDR. Nothing in the architecture ties the two together, so
 *     each core reads its own bit out of the banked GICD_ITARGETSR0 and
 *     records it (cpu_mask[]); that table is how an SGI finds a core and how
 *     an SPI is routed;
 *   * there are no LPIs, so no MSI through the GIC. PCI falls back to INTx.
 *
 * GROUPS, and why "Group 0, enable bit 0" is right on both machines:
 *   - a real Pi runs this kernel NON-SECURE, after firmware put every
 *     interrupt in Group 1. Non-secure writes to GICD_IGROUPR are ignored,
 *     and in the non-secure view bit 0 of GICD_CTLR and GICC_CTLR IS the
 *     Group 1 enable;
 *   - QEMU `virt`'s GICv2 has one security state. The IGROUPR writes stick,
 *     every line is Group 0, bit 0 is the Group 0 enable, and with FIQEn
 *     clear Group 0 is signalled as IRQ.
 * So the same writes do the right thing in both, and the driver never has to
 * know which it is in. */

#define GICD_CTLR          0x000
#define GICD_TYPER         0x004
#define GICD_IIDR          0x008
#define GICD_IGROUPR       0x080
#define GICD_ISENABLER     0x100
#define GICD_ICENABLER     0x180
#define GICD_ICPENDR       0x280
#define GICD_ICACTIVER     0x380
#define GICD_IPRIORITYR    0x400
#define GICD_ITARGETSR     0x800
#define GICD_SGIR          0xF00

#define SGIR_ALL_BUT_SELF  (1u << 24)   /* TargetListFilter = 0b01 */

#define GICC_CTLR          0x00
#define GICC_PMR           0x04
#define GICC_BPR           0x08
#define GICC_IAR           0x0C
#define GICC_EOIR          0x10

static volatile uint8_t *gicd;
static volatile uint8_t *gicc;
static uint32_t lines;
static bool ready;

/* Each core's CPU-interface bit, by dense cpu index; 0 = not up yet. */
static uint8_t cpu_mask[MAX_CPUS];

static inline uint32_t d_read(uint32_t off)             { return *(volatile uint32_t *)(gicd + off); }
static inline void     d_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(gicd + off) = v; }
static inline void     d_write8(uint32_t off, uint8_t v) { *(volatile uint8_t *)(gicd + off) = v; }
static inline uint32_t c_read(uint32_t off)             { return *(volatile uint32_t *)(gicc + off); }
static inline void     c_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(gicc + off) = v; }

/* This core's CPU-interface bit. GICD_ITARGETSR0-7 are banked and read-only:
 * each byte reads as the mask of the core that is reading. A uniprocessor
 * implementation may read them as zero, and then there is only one core to
 * route to anyway. */
static uint8_t my_mask(void) {
    uint32_t t = d_read(GICD_ITARGETSR);
    uint8_t m = (uint8_t)(t | t >> 8 | t >> 16 | t >> 24);
    return m ? m : 1;
}

static int v2_init_this_cpu(void) {
    if (!gicd)
        return -1;

    /* INTIDs 0-31, this core's own bank. */
    d_write(GICD_ICENABLER, 0xFFFFFFFFu);
    d_write(GICD_ICPENDR,   0xFFFFFFFFu);
    d_write(GICD_ICACTIVER, 0xFFFFFFFFu);
    d_write(GICD_IGROUPR,   0);                  /* Group 0 -- see the top */
    for (uint32_t i = 0; i < 32; i++)
        d_write8(GICD_IPRIORITYR + i, GIC_PRIORITY);

    cpu_mask[this_cpu()->cpu_index & (MAX_CPUS - 1)] = my_mask();

    c_write(GICC_PMR, 0xFF);                     /* accept every priority     */
    c_write(GICC_BPR, 0);                        /* no preemption grouping    */
    c_write(GICC_CTLR, 1);                       /* enable; EOImode 0, FIQEn 0 */
    __asm__ volatile("dsb sy; isb" ::: "memory");
    return 0;
}

int gicv2_probe(const struct gic_hw **out);
static const struct gic_hw gicv2_hw;

int gicv2_probe(const struct gic_hw **out) {
    static const char *const compat[] = {
        "arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a9-gic", "arm,cortex-a7-gic",
    };
    fdt_node_t node = FDT_NONE;
    for (unsigned i = 0; i < sizeof compat / sizeof compat[0] && node == FDT_NONE; i++)
        node = fdt_find_compatible(compat[i]);
    if (node == FDT_NONE)
        return -1;

    uint64_t d_base = 0, d_size = 0, c_base = 0, c_size = 0;
    if (!fdt_reg(node, 0, &d_base, &d_size) || !fdt_reg(node, 1, &c_base, &c_size)) {
        kprintf("gic: GICv2 node has no distributor/CPU interface reg\n");
        return -2;
    }
    if (c_size < 0x1000)
        c_size = 0x1000;                          /* GICC_EOIR et al. are in page 0 */

    /* Mapped at runtime rather than through boot.S's static window: that
     * window covers a different GiB on each board (docs/RPI4.md §2.2), and
     * pmm is up by now, so there is no reason to depend on which. */
    gicd = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(d_base, d_size ? d_size : 0x1000);
    gicc = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(c_base, c_size);
    if (!gicd || !gicc) {
        kprintf("gic: could not map the GICv2 registers\n");
        gicd = gicc = 0;
        return -2;
    }

    kprintf("gic: GICD %p, GICC %p [GICv2, from the device tree]\n",
            (void *)(uintptr_t)d_base, (void *)(uintptr_t)c_base);

    lines = ((d_read(GICD_TYPER) & 0x1F) + 1) * 32;
    if (lines > GIC_MAX_INTID)
        lines = GIC_MAX_INTID;
    kprintf("gic: %d interrupt lines, IIDR %p\n", (int)lines,
            (void *)(uintptr_t)d_read(GICD_IIDR));

    /* --- distributor: the shared lines, 32 and up ------------------------ */
    d_write(GICD_CTLR, 0);
    uint8_t boot = my_mask();
    for (uint32_t i = 32; i < lines; i += 32) {
        d_write(GICD_ICENABLER + (i / 32) * 4, 0xFFFFFFFFu);
        d_write(GICD_ICPENDR   + (i / 32) * 4, 0xFFFFFFFFu);
        d_write(GICD_ICACTIVER + (i / 32) * 4, 0xFFFFFFFFu);
        d_write(GICD_IGROUPR   + (i / 32) * 4, 0);
    }
    for (uint32_t i = 32; i < lines; i++) {
        d_write8(GICD_IPRIORITYR + i, GIC_PRIORITY);
        /* Every SPI to the boot core -- the same policy gicv3.c's IROUTER
         * writes express by MPIDR, expressed here as a CPU-interface mask. */
        d_write8(GICD_ITARGETSR + i, boot);
    }
    d_write(GICD_CTLR, 1);                       /* enable -- see the top */

    ready = true;
    if (v2_init_this_cpu() != 0) {
        ready = false;
        return -2;
    }

    *out = &gicv2_hw;
    return 0;
}

static void v2_enable(uint32_t intid) {
    if (!ready || intid >= lines)
        return;
    d_write(GICD_ISENABLER + (intid / 32) * 4, 1u << (intid % 32));
    __asm__ volatile("dsb sy" ::: "memory");
}

static void v2_disable(uint32_t intid) {
    if (!ready || intid >= lines)
        return;
    d_write(GICD_ICENABLER + (intid / 32) * 4, 1u << (intid % 32));
    __asm__ volatile("dsb sy" ::: "memory");
}

/* The IAR of an SGI carries the SOURCE core in bits 12:10, and the EOI must be
 * given the whole value back or it retires nothing -- which is why the raw IAR
 * travels through gic_dispatch() rather than just the INTID. */
static uint64_t v2_ack(void)               { return c_read(GICC_IAR); }
static uint32_t v2_iar_intid(uint64_t iar) { return (uint32_t)(iar & 0x3FF); }
static void     v2_eoi(uint64_t iar)       { c_write(GICC_EOIR, (uint32_t)iar); }

static bool v2_sgi_all_but_self(uint32_t sgi) {
    __asm__ volatile("dsb ishst" ::: "memory");
    d_write(GICD_SGIR, SGIR_ALL_BUT_SELF | (sgi & 0xF));
    return true;
}

static bool v2_sgi_to_cpu(uint32_t cpu, uint32_t sgi) {
    uint8_t m = cpu < MAX_CPUS ? cpu_mask[cpu] : 0;
    if (!m)
        return false;                     /* that core never brought its GIC up */
    __asm__ volatile("dsb ishst" ::: "memory");
    d_write(GICD_SGIR, ((uint32_t)m << 16) | (sgi & 0xF));
    return true;
}

static const struct gic_hw gicv2_hw = {
    .name             = "GICv2, memory-mapped CPU interface",
    .init_this_cpu    = v2_init_this_cpu,
    .enable           = v2_enable,
    .disable          = v2_disable,
    .ack              = v2_ack,
    .iar_intid        = v2_iar_intid,
    .eoi              = v2_eoi,
    .sgi_all_but_self = v2_sgi_all_but_self,
    .sgi_to_cpu       = v2_sgi_to_cpu,
};
