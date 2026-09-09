#ifndef __PCI_H__
#define __PCI_H__

#include "include/types.h"
#include <stdint.h>


// Configuration space registers(port)
/* x86's configuration-space access ports. Kept for that architecture's
 * implementation; nothing portable may use them. */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* --- the only three architecture-specific things about PCI -----------------
 *
 * Everything else in this driver -- enumeration, BAR sizing, the capability
 * walk, bus mastering -- is built on 32-bit configuration reads and writes and
 * is the same on every machine. Only HOW a config read reaches the bus differs:
 *
 *   x86_64   writes an address to port 0xCF8 and reads port 0xCFC. Two I/O
 *            instructions, and a lock would be needed if two cores ever did it
 *            at once.
 *   aarch64  has ECAM: configuration space is simply MEMORY, at
 *            base + (bus<<20 | device<<15 | function<<12 | offset). A plain
 *            load. There are no I/O ports on this architecture at all.
 *
 * The third is the MSI message itself, which is not a format at all but an
 * address to write to: on x86 it is the local APIC's doorbell
 * (0xFEE00000 | apic_id<<12) with the vector as data; on aarch64 it is the
 * GIC ITS's translater register with an EventID. Those have nothing in common
 * beyond "a write that raises an interrupt", so the message is asked for
 * rather than computed. */
uint32_t arch_pci_cfg_read32 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void     arch_pci_cfg_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                              uint32_t value);

/* Fill in the address to write and the value to write there so that writing it
 * delivers `vector` to `cpu_id`. Returns false if this machine has no way to
 * do that yet, in which case the caller must use a legacy interrupt line --
 * which is why pci_enable_msi() has always been allowed to fail. */
bool arch_pci_msi_message(uint8_t vector, uint32_t cpu_id,
                          uint64_t *out_addr, uint32_t *out_data);

// Common config space offsets
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_REVISION     0x08
#define PCI_PROG_IF      0x09
#define PCI_SUBCLASS     0x0A
#define PCI_CLASS        0x0B
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_INTERRUPT_LINE    0x3C
#define PCI_INTERRUPT_PIN    0x3D
#define PCI_CAP_PTR          0x34


// A discouvered PCI device
 struct pci_device {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
 };

struct pci_bar {
    uint64_t address;   // base physical address (or I/O port base)
    uint64_t size;     // size of the BAR (in bytes)
    bool     is_mmio;   // true if the BAR is memory-mapped false is I/O port-mapped
    bool     is_64bit;  // true if the BAR is 64-bit
    bool     prefetchable; // true if the BAR is prefetchable
    bool     valid;     // true if the BAR is valid (i.e. not zero) false if the BAR is unused(size 0)
};


// Read and size BAR. bar_index is 0-5 for BAR0-BAR5
// For 64 bit BAR, read the pair (index and index + 1)
struct pci_bar pci_read_bar(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_index);


 #define PCI_MAX_DEVICES 64

 // Read config space of a PCI device
 uint8_t  pci_read8  (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
 uint16_t pci_read16 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
 uint32_t pci_read32 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

 // Write config space of a PCI device
 void pci_write32  (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
 void pci_write16  (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);

 // Find + enable the device's MSI capability, delivering `vector` to `apic_id`.
 // Returns true on success. Bypasses IOAPIC INTx routing.
 bool pci_enable_msi(uint8_t bus, uint8_t device, uint8_t function,
                     uint8_t vector, uint8_t apic_id);
 bool pci_enable_msix(uint8_t bus, uint8_t device, uint8_t function,
                      uint8_t vector, uint8_t apic_id);

 // Enumerate all PCI devices, fill internal table, print them
 void pci_init();

/* Assign addresses to every BAR that has none, out of the given MMIO window,
 * and enable memory decoding on the devices that get one.
 *
 * NOT CALLED ON x86, and that is the interesting part: there, firmware has
 * already programmed every BAR before the kernel runs, so the addresses read
 * back are real and this would be actively harmful. On a machine booted with
 * `-kernel` there is no firmware at all -- the BARs read back as zero, the
 * device decodes nothing, and any driver that maps BAR0 maps physical address
 * zero. The kernel has to be its own PCI resource allocator.
 *
 * `window_base`/`window_size` describe host physical addresses the host bridge
 * forwards to the bus; on aarch64 they come from the device tree's `ranges`. */
void pci_assign_resources(uint64_t window_base, uint64_t window_size);

/* Connect this device's LEGACY (INTx) interrupt to `handler` and unmask it.
 * Returns false if it could not be routed, in which case the driver must poll.
 *
 * The fourth architecture-specific thing about PCI, and the least obvious.
 * x86 reads PCI_INTERRUPT_LINE -- firmware wrote the ISA IRQ number there --
 * and hands it to the 8259/IOAPIC path. aarch64 has no such number: the line
 * is nowhere in config space, because there is no firmware to have put it
 * there. It is in the DEVICE TREE, as an `interrupt-map` from
 * (device, INTx pin) to a GIC interrupt, and the driver cannot know that. */
bool arch_pci_irq_connect(const struct pci_device *dev, void (*handler)(void));

 // Access the discovered PCI devices table
 uint32_t pci_devices_count(void);
 const struct pci_device *pci_get_device(uint32_t index);

// Enable bus mastering (command register bit 2) for a device
void pci_enable_bus_mastering(uint8_t bus, uint8_t device, uint8_t function);
#endif /* __PCI_H__ */