#include "power/power.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "include/arch_irq.h"
#include "arch/aarch64/smp/smp.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/vmm.h"

/* Powering off and rebooting an aarch64 machine.
 *
 * THIS IS THE HALF THAT IS EASIER THAN x86, and the reason is architectural
 * rather than accidental. x86 has no standard way to turn a machine off that
 * does not go through ACPI and an AML interpreter, so its implementation is a
 * list of per-platform port writes. ARM has PSCI: a firmware interface,
 * described in the device tree, with SYSTEM_OFF and SYSTEM_RESET as ordinary
 * numbered calls. The kernel asks firmware and firmware does it.
 *
 * The conduit -- `hvc` or `smc` -- is whichever the device tree names, and it
 * is already probed for CPU_ON when secondaries start. This reuses that rather
 * than probing again: two implementations of "which instruction reaches
 * firmware" is two places to get it wrong, and the wrong one on a machine
 * without a hypervisor is an undefined-instruction trap in the shutdown path.
 *
 * PSCI 0.2 function IDs (DEN 0022, table 3). These are 32-bit calls -- SMC32 --
 * even on a 64-bit machine: neither takes an argument that could not fit, and
 * the specification defines only the 32-bit forms. */
#define PSCI_SYSTEM_OFF    0x84000008u
#define PSCI_SYSTEM_RESET  0x84000009u

/* --- a Raspberry Pi: no PSCI, a watchdog instead -- docs/RPI4.md -----------
 *
 * The Pi's firmware offers no PSCI. What it offers is the BCM2835 power-
 * management block's watchdog, which resets the SoC when it expires, and a
 * convention on top of it: the RSTS register's "partition" field tells the
 * firmware what to boot next -- 0 is the normal boot, 63 means HALT (stay
 * down until the power is cycled), which is the nearest a Pi gets to off.
 * Linux's drivers/watchdog/bcm2835_wdt.c does the same sequence.
 *
 * Every write to these registers must carry the 0x5a password in its top
 * byte or the block ignores it -- a write without it is not an error, just
 * nothing, which is the whole of why it is spelled out below. */
#define PM_RSTC                0x1c
#define PM_RSTS                0x20
#define PM_WDOG                0x24
#define PM_PASSWORD            0x5a000000u
#define PM_RSTC_WRCFG_CLR      0xffffffcfu
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020u
#define PM_RSTS_PARTITION_CLR  0xfffffaaau
#define PM_PARTITION_HALT      63

/* The partition number's six bits are spread over RSTS bits 0,2,4,6,8,10. */
static uint32_t rsts_partition(uint32_t p) {
    uint32_t v = 0;
    for (int b = 0; b < 6; b++)
        v |= ((p >> b) & 1u) << (2 * b);
    return v;
}

/* Reset through the watchdog, asking the firmware for `partition` next.
 * Returns only on failure: -EMBK_ENODEV when there is no watchdog (not a Pi),
 * -EMBK_EIO when there is one and the machine is somehow still running. */
static int bcm_watchdog_reset(uint32_t partition) {
    fdt_node_t n = fdt_find_compatible("brcm,bcm2835-pm-wdt");
    if (n == FDT_NONE)
        n = fdt_find_compatible("brcm,bcm2835-pm");
    uint64_t phys = 0, size = 0;
    if (n == FDT_NONE || !fdt_reg(n, 0, &phys, &size))
        return -EMBK_ENODEV;

    /* Mapped here, with interrupts still on: it may take a page-table page. */
    volatile uint8_t *pm = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(phys, size ? size : 0x100);
    if (!pm)
        return -EMBK_ENODEV;
#define PM(off) (*(volatile uint32_t *)(pm + (off)))

    arch_irq_disable();
    kprintf("power: BCM2835 watchdog reset, next boot partition %u%s\n",
            partition, partition == PM_PARTITION_HALT ? " (halt)" : "");

    PM(PM_RSTS) = PM_PASSWORD | (PM(PM_RSTS) & PM_RSTS_PARTITION_CLR) |
                  rsts_partition(partition);
    PM(PM_WDOG) = PM_PASSWORD | 10;               /* ten ticks: ~150 us */
    PM(PM_RSTC) = PM_PASSWORD | (PM(PM_RSTC) & PM_RSTC_WRCFG_CLR) |
                  PM_RSTC_WRCFG_FULL_RESET;
    __asm__ volatile("dsb sy" ::: "memory");
#undef PM

    /* A second is several thousand times the timeout. Still here after it
     * means the block did not take the write, and that should be said. */
    uint64_t frq, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t0));
    do __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t)); while (t - t0 < frq);
    kprintf("power: the watchdog did not reset the machine\n");
    return -EMBK_EIO;
}

int arch_power_off(void) {
    if (!psci_available()) {
        if (bcm_watchdog_reset(PM_PARTITION_HALT) == -EMBK_EIO)
            return -EMBK_ENODEV;                 /* a Pi, and it already said why */
        kprintf("power: no PSCI node and no watchdog in the device tree -- nothing to ask\n");
        return -EMBK_ENODEV;
    }

    /* Interrupts off: every instruction after this is meant to be the last,
     * and a timer tick would run the scheduler on a machine that is halfway
     * through powering down. */
    arch_irq_disable();

    kprintf("power: PSCI SYSTEM_OFF\n");
    int64_t rc = psci_invoke(PSCI_SYSTEM_OFF, 0, 0, 0);

    /* SYSTEM_OFF does not return. Reaching here means firmware declined --
     * PSCI's own convention, a negative status -- and that is worth reporting
     * rather than halting, because a hang looks exactly like a crash. */
    kprintf("power: SYSTEM_OFF returned %d (firmware declined)\n", (int)rc);
    return -EMBK_ENODEV;
}

int arch_power_reboot(void) {
    if (!psci_available()) {
        if (bcm_watchdog_reset(0) == -EMBK_EIO)
            return -EMBK_ENODEV;                 /* a Pi, and it already said why */
        kprintf("power: no PSCI node and no watchdog in the device tree -- nothing to ask\n");
        return -EMBK_ENODEV;
    }

    arch_irq_disable();

    kprintf("power: PSCI SYSTEM_RESET\n");
    int64_t rc = psci_invoke(PSCI_SYSTEM_RESET, 0, 0, 0);

    kprintf("power: SYSTEM_RESET returned %d (firmware declined)\n", (int)rc);
    return -EMBK_ENODEV;
}
