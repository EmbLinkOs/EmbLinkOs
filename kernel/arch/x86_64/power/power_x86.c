#include "power/power.h"
#include "include/io.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "include/arch_irq.h"

/* Powering off and rebooting an x86 machine -- the architecture's half of
 * kernel/power/power.c.
 *
 * THE HONEST SCOPE, first, because it decides what the code below is allowed
 * to claim. A GENERAL x86 power-off requires ACPI: find the FADT, read
 * PM1a_CNT_BLK, and get SLP_TYPa for state S5 out of the DSDT -- which is AML
 * bytecode, and reading it means an AML interpreter. This kernel parses the
 * MADT and the HPET table and stops there. So what follows is the set of
 * mechanisms that work WITHOUT AML, tried in order, each one documented as to
 * where it is real:
 *
 *   1. The PM1a control register at the address every mainstream emulator
 *      places it, written with SLP_TYP=S5 | SLP_EN. Real on QEMU (ICH9 and
 *      PIIX4), Bochs, and VirtualBox. Not a guess about the hardware -- a
 *      known constant of those platforms.
 *   2. The 8042 keyboard controller's CPU-reset line, for reboot. Genuinely
 *      universal on anything with a PS/2 controller, which is most x86.
 *   3. A deliberate triple fault. Every x86 reboots when a fault occurs while
 *      handling a fault while handling a fault; loading a zero-length IDT and
 *      raising an interrupt does it. The last resort, and it always works.
 *
 * On REAL hardware without ACPI parsing, power-off falls through all of it and
 * power_transition() reports that it could not -- which is the truth, and is
 * why it returns an error instead of halting and looking like a hang. The AML
 * interpreter is recorded in docs/TODO.md as what would close it. */

/* SLP_EN (bit 13) plus SLP_TYP in bits 10-12. S5 (soft off) is type 0 on QEMU's
 * ICH9 and PIIX4 and type 5 on Bochs/VirtualBox's, so both are tried: writing
 * the wrong type to the right port is a no-op, not damage. */
#define SLP_EN        (1u << 13)
#define SLP_TYP(n)    (((n) & 7u) << 10)

struct pm1a_port { uint16_t port; uint16_t value; const char *what; };

static const struct pm1a_port g_off_sequence[] = {
    { 0x0604, SLP_TYP(0) | SLP_EN, "QEMU ICH9/PIIX4 PM1a" },
    { 0xB004, SLP_TYP(0) | SLP_EN, "Bochs PM1a" },
    { 0x4004, SLP_TYP(5) | SLP_EN, "VirtualBox PM1a" },
    { 0x0600, SLP_TYP(5) | SLP_EN, "older QEMU PIIX4 PM1a" },
};

int arch_power_off(void) {
    /* Interrupts off first. Every write below is meant to be the last thing
     * this machine does; taking a timer tick between them would run the
     * scheduler on a machine that is halfway through powering down. */
    arch_irq_disable();

    for (unsigned i = 0; i < sizeof g_off_sequence / sizeof g_off_sequence[0]; i++) {
        kprintf("power: trying %s (port 0x%x)\n",
                g_off_sequence[i].what, g_off_sequence[i].port);
        outw(g_off_sequence[i].port, g_off_sequence[i].value);
        /* If that worked the machine is already gone. A short settle before
         * the next attempt: the chipset latches the write on its own clock,
         * not ours. */
        for (volatile int spin = 0; spin < 1000000; spin++) { }
    }

    return -EMBK_ENODEV;     /* caller reports it; see power_transition */
}

int arch_power_reboot(void) {
    arch_irq_disable();

    /* 1. The 8042's output port, bit 0 low = pulse RESET#. Wait for the input
     *    buffer to drain first or the command is dropped. */
    kprintf("power: rebooting via the 8042 reset line\n");
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
    }
    outb(0x64, 0xFE);

    for (volatile int spin = 0; spin < 10000000; spin++) { }

    /* 2. Triple fault. A zero-limit IDT means every vector is a
     *    descriptor-table violation, so the first interrupt faults, the fault
     *    handler faults, and the double-fault handler faults -- which no x86
     *    can recover from and every x86 answers by resetting. This is not a
     *    hack of last resort so much as the defined behaviour of the
     *    architecture, and it is why reboot never has to report failure. */
    kprintf("power: 8042 did not answer; triple-faulting\n");
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } null_idt = { 0, 0 };
    __asm__ volatile("lidt %0" :: "m"(null_idt));
    __asm__ volatile("int $0x03");

    for (;;)
        __asm__ volatile("hlt");
}
