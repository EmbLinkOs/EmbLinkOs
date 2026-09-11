#include "power/power.h"
#include "include/io.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "include/arch_irq.h"
#include "acpi/acpi.h"
#include "mm/vmm.h"                /* vmm_map_mmio: a memory-space GAS */
#include "drivers/bus/pci.h"       /* arch_pci_cfg_*: a PCI-config GAS */

/* Powering off and rebooting an x86 machine -- the architecture's half of
 * kernel/power/power.c.
 *
 * THE HONEST SCOPE, first, because it decides what the code below is allowed
 * to claim. A GENERAL x86 power-off requires ACPI: find the FADT, read
 * PM1a_CNT_BLK, and get SLP_TYPa for state S5 out of the DSDT -- which is AML
 * bytecode. kernel/acpi/acpi.c now does exactly that, by DECODING the \_S5_
 * package (a named list of integer constants on essentially every machine)
 * rather than interpreting AML. That is the mechanism tried first below, and on
 * real hardware it is the one that matters. What remains after it, in order:
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
 * A machine whose \_S5_ is a METHOD rather than a package falls through to
 * these, and power_transition() reports that it could not power off -- which
 * is the truth, and is why it returns an error instead of halting and looking
 * like a hang. The AML interpreter that would close that last gap is recorded
 * in docs/PILLARS.md. */

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

/* ---- the Generic Address Structure, acted on ------------------------------
 * ACPI names a register by (address space, width, address). These two turn
 * that into an access. Width comes from the GAS's own access size when it has
 * one and from its bit width otherwise; PM1 control is 16 bits on every PC but
 * the table is the authority, not that fact. */
static unsigned gas_bytes(const struct acpi_gas *g) {
    switch (g->access_size) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 4;
    case 4: return 8;
    default: break;
    }
    if (g->bit_width >= 32) return 4;
    if (g->bit_width >= 16) return 2;
    return 1;
}

static uint32_t gas_read(const struct acpi_gas *g) {
    unsigned n = gas_bytes(g);
    if (g->space_id == ACPI_GAS_IO) {
        uint16_t port = (uint16_t)g->address;
        return n == 1 ? inb(port) : n == 2 ? inw(port) : inl(port);
    }
    if (g->space_id == ACPI_GAS_MEMORY) {
        volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)
            vmm_map_mmio(g->address & ~0xFFFull, 0x1000);
        if (!m) return 0;
        m += g->address & 0xFFF;
        return n == 1 ? *m : n == 2 ? *(volatile uint16_t *)m : *(volatile uint32_t *)m;
    }
    return 0;
}

static void gas_write(const struct acpi_gas *g, uint32_t v) {
    unsigned n = gas_bytes(g);
    if (g->space_id == ACPI_GAS_IO) {
        uint16_t port = (uint16_t)g->address;
        if (n == 1) outb(port, (uint8_t)v);
        else if (n == 2) outw(port, (uint16_t)v);
        else outl(port, v);
    } else if (g->space_id == ACPI_GAS_MEMORY) {
        volatile uint8_t *m = (volatile uint8_t *)(uintptr_t)
            vmm_map_mmio(g->address & ~0xFFFull, 0x1000);
        if (!m) return;
        m += g->address & 0xFFF;
        if (n == 1) *m = (uint8_t)v;
        else if (n == 2) *(volatile uint16_t *)m = (uint16_t)v;
        else *(volatile uint32_t *)m = v;
    } else if (g->space_id == ACPI_GAS_PCI_CFG) {
        /* ACPI's PCI-config address: device in bits 47:32, function in 31:16,
         * register in 15:0, on bus 0. A byte write done as a dword
         * read-modify-write, because config space is dword-addressed here. */
        uint8_t dev = (uint8_t)(g->address >> 32), fn = (uint8_t)(g->address >> 16);
        uint8_t off = (uint8_t)g->address;
        uint32_t old = arch_pci_cfg_read32(0, dev, fn, off & ~3u);
        unsigned sh = (off & 3u) * 8u;
        uint32_t mask = (n >= 4) ? 0xFFFFFFFFu : (((1u << (n * 8u)) - 1u) << sh);
        arch_pci_cfg_write32(0, dev, fn, off & ~3u, (old & ~mask) | ((v << sh) & mask));
    }
}

static void settle(void) {
    /* The chipset latches a power-control write on its own clock, not ours. */
    for (volatile int spin = 0; spin < 1000000; spin++) { }
}

/* S5 THE WAY THE MACHINE SAYS TO. Returns only if the machine is still here. */
static void acpi_s5(const struct acpi_power_info *pi) {
    if (pi->hw_reduced) {
        /* ACPI 5 hardware-reduced: one sleep-control register, SLP_TYP in bits
         * 4:2 and SLP_EN at bit 5. No PM1 block, no SMI handshake. */
        if (!pi->sleep_control.address) {
            kprintf("power: hardware-reduced ACPI with no sleep-control register\n");
            return;
        }
        kprintf("power: ACPI S5 via the sleep-control register (SLP_TYP %u)\n",
                (unsigned)pi->s5_typa);
        gas_write(&pi->sleep_control, ((uint32_t)(pi->s5_typa & 7u) << 2) | (1u << 5));
        settle();
        return;
    }
    if (!pi->pm1a_cnt.address) {
        kprintf("power: the FADT has no PM1a control block\n");
        return;
    }

    /* ACPI MODE FIRST. Firmware may leave the chipset in legacy (SMM) mode,
     * where the PM1 registers belong to the BIOS and a write to them is not
     * guaranteed to reach the power logic. SCI_EN (bit 0 of PM1 control)
     * reports which mode we are in; the FADT's SMI command switches it. */
    if (!(gas_read(&pi->pm1a_cnt) & 1u) && pi->smi_cmd && pi->acpi_enable) {
        outb((uint16_t)pi->smi_cmd, pi->acpi_enable);
        for (int i = 0; i < 3000000 && !(gas_read(&pi->pm1a_cnt) & 1u); i++) { }
        kprintf("power: chipset switched to ACPI mode (SCI_EN %s)\n",
                (gas_read(&pi->pm1a_cnt) & 1u) ? "set" : "STILL CLEAR");
    }

    kprintf("power: ACPI S5 via PM1a_CNT 0x%llx (SLP_TYPa %u%s)\n",
            (unsigned long long)pi->pm1a_cnt.address, (unsigned)pi->s5_typa,
            pi->pm1b_cnt.address ? ", and PM1b" : "");

    /* The type first and SLP_EN second, as two writes: some chipsets sample
     * SLP_TYP on the rising edge of SLP_EN and ignore a type that arrives in
     * the same write. PM1a and PM1b both, because on a machine that has both
     * the power logic listens to the pair. */
    uint32_t a = gas_read(&pi->pm1a_cnt) & ~SLP_TYP(7) & ~SLP_EN;
    uint32_t b = pi->pm1b_cnt.address ? (gas_read(&pi->pm1b_cnt) & ~SLP_TYP(7) & ~SLP_EN) : 0;
    gas_write(&pi->pm1a_cnt, a | SLP_TYP(pi->s5_typa));
    if (pi->pm1b_cnt.address)
        gas_write(&pi->pm1b_cnt, b | SLP_TYP(pi->s5_typb));
    gas_write(&pi->pm1a_cnt, a | SLP_TYP(pi->s5_typa) | SLP_EN);
    if (pi->pm1b_cnt.address)
        gas_write(&pi->pm1b_cnt, b | SLP_TYP(pi->s5_typb) | SLP_EN);
    settle();
}

int arch_power_off(void) {
    /* Interrupts off first. Every write below is meant to be the last thing
     * this machine does; taking a timer tick between them would run the
     * scheduler on a machine that is halfway through powering down. */
    arch_irq_disable();

    /* 1. What the machine's own tables say. On real hardware this is the only
     *    mechanism that works; on an emulator it is the same register the
     *    constants below would have hit, reached for the right reason. */
    const struct acpi_power_info *pi = acpi_power_info();
    if (pi && pi->fadt_found && pi->s5_found) {
        acpi_s5(pi);
        kprintf("power: ACPI S5 did not take the machine\n");
    } else {
        kprintf("power: ACPI gave no S5 (%s) -- trying known emulator registers\n",
                !pi ? "no ACPI tables" : !pi->fadt_found ? "no FADT" : "no \\_S5_ package");
    }

    /* 2. The emulator constants: last resort, kept because they are true of
     *    the platforms they name and cost nothing to try. */
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

    /* 0. The FADT's reset register -- the mechanism the machine itself
     *    declares, and on a modern PC with no PS/2 controller the only one
     *    that is not a deliberate crash. Usually port 0xCF9, value 6. */
    const struct acpi_power_info *pi = acpi_power_info();
    if (pi && pi->reset_supported) {
        kprintf("power: rebooting via the ACPI reset register (0x%llx <- 0x%x)\n",
                (unsigned long long)pi->reset_reg.address, (unsigned)pi->reset_value);
        gas_write(&pi->reset_reg, pi->reset_value);
        for (volatile int spin = 0; spin < 10000000; spin++) { }
        kprintf("power: the ACPI reset register did not reset the machine\n");
    }

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
