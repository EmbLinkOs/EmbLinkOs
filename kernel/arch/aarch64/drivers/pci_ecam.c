#include "drivers/bus/pci.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/vmm.h"
#include "include/kprintf.h"

/* PCIe configuration space on aarch64 -- ECAM. docs/ARM64.md phase A7.
 *
 * There are no I/O ports on this architecture, so the x86 CF8/CFC dance has no
 * counterpart. What it has instead is simpler: configuration space IS MEMORY.
 * A device's config registers live at
 *
 *     ecam_base + (bus << 20) | (device << 15) | (function << 12) | offset
 *
 * so a config read is a load, needs no lock, and cannot race with another
 * core the way a two-port sequence can.
 *
 * The base comes from the device tree (`pci-host-ecam-generic`), not a
 * constant: QEMU `virt` puts it at 0x4010000000 today and that is not
 * something to hardcode when the tree is right there. */

static volatile uint8_t *ecam;
static uint64_t ecam_size;

/* Config space is mapped Device-nGnRnE, like any other register window.
 * Deliberately NOT Normal memory: a configuration write that got merged,
 * reordered or acknowledged early would be a device programmed in the wrong
 * order, which is exactly the failure that is impossible to reproduce. */
static void ecam_init(void) {
    if (ecam)
        return;

    fdt_node_t n = fdt_find_compatible("pci-host-ecam-generic");
    if (n == FDT_NONE) {
        kprintf("pci: no ECAM host bridge in the device tree\n");
        return;
    }

    uint64_t base = 0, size = 0;
    if (!fdt_reg(n, 0, &base, &size) || !size) {
        kprintf("pci: ECAM node has no usable reg\n");
        return;
    }

    uint64_t va = vmm_map_mmio(base, size);
    if (!va) {
        kprintf("pci: could not map %d MiB of ECAM at %p\n",
                (int)(size >> 20), (void *)(uintptr_t)base);
        return;
    }

    ecam = (volatile uint8_t *)(uintptr_t)va;
    ecam_size = size;
    kprintf("pci: ECAM at %p (%d MiB), mapped to %p [from the device tree]\n",
            (void *)(uintptr_t)base, (int)(size >> 20), (void *)(uintptr_t)va);
}

static volatile uint32_t *cfg_word(uint8_t bus, uint8_t device, uint8_t function,
                                   uint8_t offset) {
    ecam_init();
    if (!ecam)
        return 0;

    uint64_t off = ((uint64_t)bus << 20) | ((uint64_t)device << 15) |
                   ((uint64_t)function << 12) | (offset & 0xFCu);

    /* A bus number past the window would read whatever follows the mapping. */
    if (off + 4 > ecam_size)
        return 0;

    return (volatile uint32_t *)(ecam + off);
}

uint32_t arch_pci_cfg_read32(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset) {
    volatile uint32_t *p = cfg_word(bus, device, function, offset);
    /* All-ones is what a real bus returns for an absent device, so it is also
     * the right answer when there is no ECAM window at all -- enumeration
     * treats it as "nothing here" and moves on. */
    return p ? *p : 0xFFFFFFFFu;
}

void arch_pci_cfg_write32(uint8_t bus, uint8_t device, uint8_t function,
                          uint8_t offset, uint32_t value) {
    volatile uint32_t *p = cfg_word(bus, device, function, offset);
    if (p)
        *p = value;
}

bool arch_pci_msi_message(uint8_t vector, uint32_t cpu_id,
                          uint64_t *out_addr, uint32_t *out_data) {
    (void)vector; (void)cpu_id; (void)out_addr; (void)out_data;

    /* No MSI yet, and saying so is the correct behaviour rather than a gap.
     *
     * On this architecture an MSI is a write to the GIC's ITS translater
     * register carrying an EventID, and the ITS needs its own command queue,
     * device table and interrupt-translation tables set up first. None of that
     * exists (docs/TODO.md). Returning false makes pci_enable_msi() fail,
     * which every caller already handles by falling back to a legacy
     * interrupt line -- a path that works and is what `virt` provides anyway.
     *
     * The alternative -- programming an address that means nothing here --
     * would produce a device that appears to be configured and never
     * interrupts. */
    return false;
}

/* --- the MMIO window, from the device tree --------------------------------
 *
 * The host bridge's `ranges` says which host-physical addresses it forwards
 * onto the bus, and in which address space. Each entry is seven cells:
 *
 *     3  child address   phys.hi, phys.mid, phys.lo
 *     2  parent address  where those appear in host physical space
 *     2  size
 *
 * and phys.hi bits 25:24 select the space: 01 = I/O, 10 = 32-bit memory,
 * 11 = 64-bit memory. We want the 32-bit memory window, because a BAR that is
 * not 64-bit-capable can only be placed below 4 GiB and every virtio device on
 * `virt` has one. */
void pci_ecam_assign_resources(void) {
    ecam_init();

    fdt_node_t n = fdt_find_compatible("pci-host-ecam-generic");
    if (n == FDT_NONE)
        return;

    uint32_t len = 0;
    const uint8_t *p = (const uint8_t *)fdt_prop(n, "ranges", &len);
    if (!p) {
        kprintf("pci: host bridge has no `ranges`; cannot place BARs\n");
        return;
    }

    /* Seven cells of four bytes each. Read big-endian by hand rather than
     * casting: `ranges` is only 4-byte aligned and the 64-bit fields inside it
     * routinely are not. */
    for (uint32_t off = 0; off + 28 <= len; off += 28) {
        const uint8_t *e = p + off;

        uint32_t hi = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) |
                      ((uint32_t)e[2] << 8)  |  (uint32_t)e[3];
        uint32_t space = (hi >> 24) & 0x3;
        if (space != 0x2)                     /* 32-bit memory only */
            continue;

        uint64_t parent = 0, size = 0;
        for (int i = 0; i < 8; i++) parent = (parent << 8) | e[12 + i];
        for (int i = 0; i < 8; i++) size   = (size   << 8) | e[20 + i];

        kprintf("pci: 32-bit MMIO window %p + %d MiB [from the device tree]\n",
                (void *)(uintptr_t)parent, (int)(size >> 20));
        pci_assign_resources(parent, size);
        return;
    }

    kprintf("pci: no 32-bit MMIO window in `ranges`\n");
}
