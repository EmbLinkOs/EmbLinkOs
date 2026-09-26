#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/irq/gic_hw.h"
#include "arch/aarch64/cpu/percpu.h"
#include "arch/aarch64/boot/fdt.h"
#include "include/kprintf.h"

/* What every GIC does -- see gic_hw.h. The hardware is behind `hw`, which
 * gic_init() chooses from the device tree: a GICv3 (QEMU `virt`) or a GICv2
 * (the Raspberry Pi 4's GIC-400, or `virt,gic-version=2`).
 *
 * Priorities: one, 0x80 (GIC_PRIORITY), for everything. A single priority
 * means no interrupt can preempt another, which is what we want while the
 * kernel runs with a single IRQ stack: nesting needs a stack per level and
 * there is exactly one. The GIC only implements the HIGH bits of the field,
 * so 0x80 is chosen because it is representable on every implementation. */

static const struct gic_hw *hw;

static struct {
    gic_handler_t fn;
    const char   *name;
    uint64_t      count;
} handlers[GIC_MAX_INTID];

static uint64_t spurious;
static void (*post_eoi)(void);

int gic_init(void) {
    int rc = gicv3_probe(&hw);
    if (rc == -1)
        rc = gicv2_probe(&hw);

    if (rc == 0) {
        kprintf("gic: initialised (%s)\n", hw->name);
        return 0;
    }
    hw = 0;
    if (rc == -1)
        kprintf("gic: the device tree has no GICv3 (arm,gic-v3) and no GICv2 "
                "(arm,gic-400 / arm,cortex-a15-gic)\n");
    return -1;
}

int gic_init_this_cpu(void) {
    if (!hw) {
        kprintf("gic: gic_init_this_cpu() before gic_init()\n");
        return -1;
    }
    return hw->init_this_cpu();
}

void gic_enable(uint32_t intid) {
    if (hw && intid < GIC_MAX_INTID)
        hw->enable(intid);
}

void gic_disable(uint32_t intid) {
    if (hw && intid < GIC_MAX_INTID)
        hw->disable(intid);
}

int gic_register(uint32_t intid, gic_handler_t handler, const char *name) {
    if (intid >= GIC_MAX_INTID || !handler)
        return -1;

    handlers[intid].fn    = handler;
    handlers[intid].name  = name ? name : "?";
    handlers[intid].count = 0;
    gic_enable(intid);

    kprintf("gic: INTID %d -> %s\n", (int)intid, handlers[intid].name);
    return 0;
}

void gic_unregister(uint32_t intid) {
    if (intid >= GIC_MAX_INTID)
        return;
    gic_disable(intid);
    handlers[intid].fn = 0;
}

void gic_set_post_eoi(void (*fn)(void)) {
    post_eoi = fn;
}

bool gic_send_sgi_all_but_self(uint32_t sgi) {
    return hw && sgi < 16 && hw->sgi_all_but_self(sgi);
}

bool gic_send_sgi(uint32_t cpu_index, uint32_t sgi) {
    return hw && sgi < 16 && hw->sgi_to_cpu(cpu_index, sgi);
}

/* Per-core interrupt tally, for diagnosing "does this core take interrupts at
 * all" -- a question that is otherwise invisible, because every count the GIC
 * driver keeps is shared. */
static volatile uint64_t percpu_irqs[MAX_CPUS];
uint64_t gic_percpu_irq_count(uint32_t cpu) {
    return cpu < MAX_CPUS ? percpu_irqs[cpu] : 0;
}

void gic_dispatch(void) {
    if (!hw)
        return;

    uint64_t iar = hw->ack();
    percpu_irqs[this_cpu()->cpu_index & (MAX_CPUS - 1)]++;
    uint32_t intid = hw->iar_intid(iar);

    /* An LPI -- an MSI that a GICv3's ITS translated and delivered. Handled
     * before the spurious check, because 8192+ is far above GIC_MAX_INTID and
     * would otherwise be counted as spurious and never acknowledged. A GICv2
     * has no LPIs; its INTIDs are 10 bits and cannot reach here. */
    if (intid >= GIC_LPI_BASE) {
        gicv3_lpi_dispatch(intid);
        /* EOI regardless: an LPI nobody claimed still has to be retired, or
         * the CPU interface stays busy at that priority and every later
         * interrupt is blocked behind it. */
        hw->eoi(iar);
        if (post_eoi)
            post_eoi();
        return;
    }

    if (intid >= GIC_MAX_INTID) {      /* 1020-1023: spurious or special */
        spurious++;
        return;                        /* no EOI for a spurious ID       */
    }

    /* THE ORDER OF THE NEXT THREE STEPS IS THE WHOLE DESIGN, and getting it
     * wrong produces two different bugs that look nothing like each other.
     *
     * 1. HANDLER FIRST, so the device stops asserting. A PPI like the generic
     *    timer is LEVEL-triggered: the timer holds the line high until
     *    CNTV_TVAL is reloaded. End the interrupt while the line is still
     *    high and the GIC immediately latches another one -- the timer then
     *    fires faster than it was programmed to (measured: 6.4 ms for a 10 ms
     *    tick under HVF), and the extra interrupt arrives at a moment nothing
     *    expects.
     *
     * 2. THEN EOI, retiring the interrupt completely.
     *
     * 3. THEN the post-EOI hook -- the scheduler. It must come after the EOI
     *    because it CONTEXT-SWITCHES and does not return: with the EOI still
     *    pending, the interrupt would stay active until this thread was
     *    scheduled again, the GIC would refuse to deliver another at the same
     *    priority meanwhile, and the other thread would never get a tick --
     *    one preemption, then silence.
     *
     * Steps 1 and 3 pull in opposite directions, which is exactly why the
     * scheduler is a separate hook here rather than something the timer
     * handler calls. Switching inside the handler satisfies 3 only by
     * breaking 1. */
    handlers[intid].count++;
    if (handlers[intid].fn)
        handlers[intid].fn(intid);
    else
        kprintf("gic: unhandled INTID %d\n", (int)intid);

    hw->eoi(iar);

    if (post_eoi)
        post_eoi();
}

uint64_t gic_count(uint32_t intid) {
    return intid < GIC_MAX_INTID ? handlers[intid].count : 0;
}

uint64_t gic_spurious_count(void) { return spurious; }

void gic_dump(void) {
    kprintf("gic: registered lines (%s):\n", hw ? hw->name : "no controller");
    for (uint32_t i = 0; i < GIC_MAX_INTID; i++)
        if (handlers[i].fn)
            kprintf("gic:   INTID %-4d %-16s %d deliveries\n",
                    (int)i, handlers[i].name, (int)handlers[i].count);
    kprintf("gic:   spurious: %d\n", (int)spurious);
}
