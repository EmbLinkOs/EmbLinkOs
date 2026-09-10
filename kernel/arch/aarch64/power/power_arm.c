#include "power/power.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "include/arch_irq.h"
#include "arch/aarch64/smp/smp.h"

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

int arch_power_off(void) {
    if (!psci_available()) {
        kprintf("power: no PSCI node in the device tree -- nothing to ask\n");
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
        kprintf("power: no PSCI node in the device tree -- nothing to ask\n");
        return -EMBK_ENODEV;
    }

    arch_irq_disable();

    kprintf("power: PSCI SYSTEM_RESET\n");
    int64_t rc = psci_invoke(PSCI_SYSTEM_RESET, 0, 0, 0);

    kprintf("power: SYSTEM_RESET returned %d (firmware declined)\n", (int)rc);
    return -EMBK_ENODEV;
}
