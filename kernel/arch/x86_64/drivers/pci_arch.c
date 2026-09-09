#include "drivers/bus/pci.h"
#include "include/io.h"
#include "arch/x86_64/irq/irq.h"

/* The x86_64 half of the three PCI seams in drivers/bus/pci.h.
 *
 * Configuration space is reached through two I/O PORTS, which is precisely the
 * part that does not survive a change of architecture: aarch64 has no I/O
 * ports at all. */

static uint32_t pci_config_address(uint8_t bus, uint8_t device, uint8_t function,
                                   uint8_t offset) {
    /* Enable bit (31) | bus | device | function | dword-aligned offset. */
    return (uint32_t)((1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                      ((uint32_t)function << 8) | (offset & 0xFC));
}

uint32_t arch_pci_cfg_read32(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void arch_pci_cfg_write32(uint8_t bus, uint8_t device, uint8_t function,
                          uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

bool arch_pci_msi_message(uint8_t vector, uint32_t cpu_id,
                          uint64_t *out_addr, uint32_t *out_data) {
    /* The local APIC's doorbell. Writing `vector` to this address is what makes
     * the interrupt appear -- an MSI is literally a memory write aimed at the
     * interrupt controller, which is why the "message" differs completely
     * between machines while the capability walk around it does not. */
    *out_addr = 0xFEE00000ULL | ((uint64_t)cpu_id << 12);
    *out_data = vector;
    return true;
}

bool arch_pci_irq_connect(const struct pci_device *dev, void (*handler)(void)) {
    /* Firmware wrote the ISA IRQ number into config space before the kernel
     * started, so the line is simply there to be read. That is the whole
     * difference from aarch64, where nothing put it anywhere and it has to
     * come out of the device tree. */
    uint8_t line = pci_read8(dev->bus, dev->device, dev->function, PCI_INTERRUPT_LINE);
    if (line >= 16)
        return false;

    irq_register(line, handler);
    return true;
}

uint32_t arch_pci_irq_line(const struct pci_device *dev) {
    uint8_t line = pci_read8(dev->bus, dev->device, dev->function, PCI_INTERRUPT_LINE);
    return (line < 16) ? line : 0;
}
