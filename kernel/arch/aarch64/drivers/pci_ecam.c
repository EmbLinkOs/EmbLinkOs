#include "drivers/bus/pci.h"
#include "arch/aarch64/boot/fdt.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "arch/aarch64/irq/gicv3.h"

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

/* Big-endian, unaligned-safe. Local rather than exported from fdt.c: one
 * caller does not justify widening that interface. */
static uint32_t be32_at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* INTx handlers take no arguments; the GIC hands its handlers an INTID. Rather
 * than cast between the two function types -- which is undefined behaviour the
 * moment it is CALLED, not merely converted -- keep a small table and
 * trampoline through it. Four entries because there are four INTx pins. */
#define MAX_INTX 8
static struct { uint32_t intid; void (*fn)(void); } intx[MAX_INTX];
static uint32_t intx_used;

static void pci_intx_trampoline(uint32_t intid) {
    for (uint32_t i = 0; i < intx_used; i++)
        if (intx[i].intid == intid && intx[i].fn) {
            intx[i].fn();
            return;
        }
}

/* --- legacy interrupt routing, from the device tree ------------------------
 *
 * A PCI device's INTx pin reaches the GIC through the host bridge's
 * `interrupt-map`, and there is no other way to find out which line it lands
 * on: config space's PCI_INTERRUPT_LINE is meaningless here because no
 * firmware ran to fill it in.
 *
 * The binding (Open Firmware PCI Bus Binding, §"Interrupt Mapping") is a table
 * of entries, each laid out as
 *
 *     <#address-cells of the PCI node>       child unit address  -- 3
 *     <#interrupt-cells of the PCI node>     child interrupt     -- 1, the INTx pin
 *     1 cell                                 parent phandle
 *     <#address-cells of the PARENT>         parent unit address
 *     <#interrupt-cells of the PARENT>       parent interrupt
 *
 * and `interrupt-map-mask` says which bits of the child key to compare -- for
 * PCI that is a couple of device-number bits and the pin, because slots are
 * wired in a rotating pattern and the function number is irrelevant.
 *
 * THE STRIDE MUST BE COMPUTED, NOT ASSUMED. The first version of this assumed
 * the interrupt controller had no unit address (`#address-cells = 0`, which is
 * common) and used an 8-cell stride. QEMU `virt`'s GIC declares
 * `#address-cells = <2>`, so entries are 10 cells, and the 8-cell walk
 * misaligned after the first entry -- every device then matched the same stale
 * bytes and was routed to SPI 0. It looked like it worked: two devices, two
 * "routed" messages, one wrong answer each. The fix is to read all four cell
 * counts out of the tree, which is also what makes this correct on a board
 * that is not this one.
 *
 * Decoding it at all, rather than using `virt`'s well-known formula
 * (SPI 3 + (slot + pin - 1) % 4), is the difference between a driver that
 * works on this board and one that works on the board.
 */
uint32_t arch_pci_irq_line(const struct pci_device *dev) {
    fdt_node_t n = fdt_find_compatible("pci-host-ecam-generic");
    if (n == FDT_NONE)
        return false;

    uint32_t mlen = 0, klen = 0;
    const uint8_t *map  = (const uint8_t *)fdt_prop(n, "interrupt-map", &mlen);
    const uint8_t *mask = (const uint8_t *)fdt_prop(n, "interrupt-map-mask", &klen);
    if (!map || !mask || klen < 16) {
        kprintf("pci: host bridge has no interrupt-map; %d:%d.%d cannot interrupt\n",
                dev->bus, dev->device, dev->function);
        return 0;
    }

    /* Every one of these comes from the tree. See the note above on why
     * assuming any of them is how a routing table walks off its own entries. */
    fdt_node_t gic = fdt_find_compatible("arm,gic-v3");
    uint32_t child_ac  = fdt_prop_u32(n, "#address-cells", 3);
    uint32_t child_ic  = fdt_prop_u32(n, "#interrupt-cells", 1);
    uint32_t parent_ac = (gic != FDT_NONE) ? fdt_prop_u32(gic, "#address-cells", 0) : 0;
    uint32_t parent_ic = (gic != FDT_NONE) ? fdt_prop_u32(gic, "#interrupt-cells", 3) : 3;

    uint32_t stride_cells = child_ac + child_ic + 1 + parent_ac + parent_ic;
    uint32_t stride = stride_cells * 4;
    uint32_t pirq_off = (child_ac + child_ic + 1 + parent_ac) * 4;

    if (child_ac < 1 || child_ic < 1 || parent_ic < 2 || stride_cells > 32) {
        kprintf("pci: interrupt-map cell counts look wrong (%d/%d/%d/%d)\n",
                (int)child_ac, (int)child_ic, (int)parent_ac, (int)parent_ic);
        return 0;
    }

    uint8_t pin = pci_read8(dev->bus, dev->device, dev->function, PCI_INTERRUPT_PIN);
    if (pin == 0 || pin > 4)
        return 0;                         /* the device declares no INTx pin */

    /* The child key this device presents, masked the way the bridge asks. */
    uint32_t phys_hi = ((uint32_t)dev->bus << 16) |
                       ((uint32_t)dev->device << 11) |
                       ((uint32_t)dev->function << 8);
    uint32_t m_hi  = be32_at(mask + 0);
    uint32_t m_irq = be32_at(mask + child_ac * 4);
    uint32_t key_hi  = phys_hi & m_hi;
    uint32_t key_irq = (uint32_t)pin & m_irq;

    for (uint32_t off = 0; off + stride <= mlen; off += stride) {
        const uint8_t *e = map + off;
        if ((be32_at(e + 0) & m_hi) != key_hi)
            continue;
        if ((be32_at(e + child_ac * 4) & m_irq) != key_irq)
            continue;

        /* The GIC's own interrupt specifier: {type, number, flags}. */
        uint32_t type   = be32_at(e + pirq_off);
        uint32_t number = be32_at(e + pirq_off + 4);
        kprintf("pci: %d:%d.%d INT%c -> %s %d (INTID %d) [from the device tree]\n",
                dev->bus, dev->device, dev->function, 'A' + pin - 1,
                type == GIC_TYPE_PPI ? "PPI" : "SPI", (int)number,
                (int)gic_intid(type, number));
        return gic_intid(type, number);
    }

    kprintf("pci: no interrupt-map entry for %d:%d.%d INT%c\n",
            dev->bus, dev->device, dev->function, 'A' + pin - 1);
    return 0;
}

bool arch_pci_irq_connect(const struct pci_device *dev, void (*handler)(void)) {
    uint32_t intid = arch_pci_irq_line(dev);
    if (!intid || !handler)
        return false;

    if (intx_used >= MAX_INTX) {
        kprintf("pci: too many INTx handlers\n");
        return false;
    }
    intx[intx_used].intid = intid;
    intx[intx_used].fn    = handler;
    intx_used++;

    /* Shared line: several devices can land on the same SPI, and the
     * trampoline calls whichever of them registered it. */
    gic_register(intid, pci_intx_trampoline, "pci INTx");
    return true;
}

/* How many DISTINCT interrupt lines the routed devices ended up on.
 *
 * Exists because of a specific bug: an interrupt-map walk with the wrong
 * stride routed every device to the same line and reported success for each.
 * "Two devices routed" was true and useless; "two devices on two lines" is the
 * claim that fails when the walk is wrong. */
uint32_t pci_ecam_intx_distinct(void) {
    uint32_t distinct = 0;
    for (uint32_t i = 0; i < intx_used; i++) {
        bool seen = false;
        for (uint32_t j = 0; j < i; j++)
            if (intx[j].intid == intx[i].intid) { seen = true; break; }
        if (!seen)
            distinct++;
    }
    return distinct;
}
