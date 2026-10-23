#include "arch/aarch64/drivers/pcie_brcmstb.h"
#include "arch/aarch64/drivers/bcm_mbox.h"
#include "arch/aarch64/boot/fdt.h"
#include "drivers/bus/pci.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "include/kprintf.h"
#include "mm/vmm.h"

/* The Raspberry Pi 4's PCIe host bridge (BCM2711, "brcm,bcm2711-pcie") --
 * docs/RPI4.md P5.
 *
 * WHY IT MATTERS: the Pi 4's USB ports are not the SoC's. They are a VIA
 * VL805 xHCI on the far side of this PCIe link, so without it there is no
 * keyboard and no mouse. Unlike `virt`'s ECAM, nothing has brought this
 * controller up before the kernel runs: the kernel resets it, trains the link
 * and opens its windows itself.
 *
 * HOW, and where it comes from: the sequence and the register map are Linux's
 * drivers/pci/controller/pcie-brcmstb.c for the BCM2711 (its register offsets
 * are the 2711's own, 0x9210 / 0x9000 / 0x8000 below). Nothing here could be
 * tested before real hardware -- QEMU's raspi4b has no PCIe at all -- so every
 * step says what it did and what it read back, on the screen, and the first
 * one to fail stops with the evidence. A photograph of that screen is the bug
 * report.
 *
 * CONFIGURATION SPACE is two-level:
 *   bus 0, device 0  the root port itself -- its registers ARE the start of
 *                    this block, `base + offset`;
 *   bus 1..          through a window: write bus/device/function to
 *                    EXT_CFG_INDEX, then access EXT_CFG_DATA + offset.
 * A PCIe link is point to point, so only device 0 exists on either bus, and
 * the Pi 4 has nothing below the VL805; every other address reads all-ones
 * without being asked of the hardware. */

/* Register offsets, BCM2711 (pcie-brcmstb.c). */
#define RC_CFG_VENDOR_SPECIFIC_REG1   0x0188
#define  REG1_ENDIAN_MODE_BAR2_MASK   0x0000000Cu   /* 0 = little endian */
#define RC_CFG_PRIV1_ID_VAL3          0x043C
#define  ID_VAL3_CLASS_CODE_MASK      0x00FFFFFFu
#define BRCM_PCIE_CAP_REGS            0x00AC        /* the PCIe capability */
#define  PCI_EXP_LNKSTA               0x12
#define MISC_MISC_CTRL                0x4008
#define  MISC_CTRL_SCB_ACCESS_EN      0x00001000u
#define  MISC_CTRL_CFG_READ_UR_MODE   0x00002000u
#define  MISC_CTRL_MAX_BURST_MASK     0x00300000u   /* 0 = 128 bytes on the 2711 */
#define  MISC_CTRL_SCB0_SIZE_MASK     0xF8000000u
#define MISC_CPU_2_PCIE_MEM_WIN0_LO   0x400C
#define MISC_CPU_2_PCIE_MEM_WIN0_HI   0x4010
#define MISC_RC_BAR1_CONFIG_LO        0x402C
#define MISC_RC_BAR2_CONFIG_LO        0x4034
#define MISC_RC_BAR2_CONFIG_HI        0x4038
#define MISC_RC_BAR3_CONFIG_LO        0x403C
#define  RC_BAR_SIZE_MASK             0x0000001Fu
#define MISC_PCIE_STATUS              0x4068
#define  STATUS_PORT_RC               0x00000080u
#define  STATUS_DL_ACTIVE             0x00000020u
#define  STATUS_PHYLINKUP             0x00000010u
#define MISC_REVISION                 0x406C
#define MISC_WIN0_BASE_LIMIT          0x4070
#define  WIN_BASE_MASK                0x0000FFF0u
#define  WIN_LIMIT_MASK               0xFFF00000u
#define MISC_WIN0_BASE_HI             0x4080
#define MISC_WIN0_LIMIT_HI            0x4084
#define  WIN_HI_MASK                  0x000000FFu
#define MISC_HARD_PCIE_HARD_DEBUG     0x4204
#define  HARD_DEBUG_CLKREQ_DEBUG_EN   0x00000002u
#define  HARD_DEBUG_SERDES_IDDQ       0x08000000u
#define MSI_INTR2_BASE                0x4500
#define  INTR2_CLR                    0x08
#define  INTR2_MASK_SET               0x10
#define EXT_CFG_DATA                  0x8000
#define EXT_CFG_INDEX                 0x9000
#define RGR1_SW_INIT_1                0x9210
#define  SW_INIT_1_PERST              0x00000001u
#define  SW_INIT_1_INIT               0x00000002u

/* Type-1 (bridge) header, for the root port. */
#define PCI_COMMAND                   0x04
#define PCI_BUS_NUMBERS               0x18   /* primary | secondary<<8 | subordinate<<16 */
#define PCI_MEM_BASE_LIMIT            0x20
#define PCI_PREF_BASE_LIMIT           0x24

static volatile uint8_t *base;
static bool     g_up;
static uint64_t g_win_cpu, g_win_bus, g_win_size;     /* outbound: CPU -> PCI */
static spinlock_t g_cfg_lock = SPINLOCK_INIT;

static inline uint32_t rd(uint32_t off)             { return *(volatile uint32_t *)(base + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; }

/* Linux's u32p_replace_bits: put `val` in the field `mask` describes. */
static uint32_t field(uint32_t word, uint32_t mask, uint32_t val) {
    uint32_t shift = (uint32_t)__builtin_ctz(mask);
    return (word & ~mask) | ((val << shift) & mask);
}

static uint32_t ilog2_64(uint64_t v) { return 63u - (uint32_t)__builtin_clzll(v); }

/* The inbound (PCIe -> memory) window's size field: log2 of the size, encoded
 * two different ways depending on the range (pcie-brcmstb.c
 * brcm_pcie_encode_ibar_size). */
static uint32_t encode_ibar_size(uint64_t size) {
    uint32_t l = ilog2_64(size);
    if (l >= 12 && l <= 15) return (l - 12) + 0x1C;
    if (l >= 16 && l <= 35) return l - 15;
    return 0;
}

static bool link_up(void) {
    uint32_t s = rd(MISC_PCIE_STATUS);
    return (s & STATUS_DL_ACTIVE) && (s & STATUS_PHYLINKUP);
}

static uint64_t be_cells(const uint8_t *p, int cells) {
    uint64_t v = 0;
    for (int i = 0; i < cells * 4; i++) v = (v << 8) | p[i];
    return v;
}

/* The first 32-bit-memory entry of `ranges` (outbound) or `dma-ranges`
 * (inbound): 3 PCI address cells, 2 parent cells, 2 size cells. */
static bool read_window(fdt_node_t n, const char *prop, uint64_t *pci,
                        uint64_t *parent, uint64_t *size) {
    uint32_t len = 0;
    const uint8_t *p = (const uint8_t *)fdt_prop(n, prop, &len);
    for (uint32_t off = 0; p && off + 28 <= len; off += 28) {
        uint32_t space = (uint32_t)(be_cells(p + off, 1) >> 24) & 3u;
        if (space != 2 && space != 3) continue;         /* memory, 32- or 64-bit */
        *pci    = be_cells(p + off + 4, 2);
        *parent = be_cells(p + off + 12, 2);
        *size   = be_cells(p + off + 20, 2);
        return true;
    }
    return false;
}

bool brcm_pcie_init(void) {
    fdt_node_t n = fdt_find_compatible("brcm,bcm2711-pcie");
    if (n == FDT_NONE)
        return false;                                   /* not a Pi 4: silent */
    uint32_t slen = 0;
    if (fdt_prop(n, "status", &slen) && !fdt_prop_has_string(n, "status", "okay") &&
        !fdt_prop_has_string(n, "status", "ok")) {
        /* The firmware enables it on every Pi 4; QEMU's raspi4b, which has no
         * PCIe to offer, disables it. Either way the tree has the last word. */
        kprintf("pcie: brcm,bcm2711-pcie is disabled in the device tree\n");
        return false;
    }

    uint64_t phys = 0, size = 0;
    if (!fdt_reg(n, 0, &phys, &size)) {
        kprintf("pcie: brcm,bcm2711-pcie has no usable reg\n");
        return false;
    }
    uint64_t in_pci = 0, in_cpu = 0, in_size = 0;
    if (!read_window(n, "ranges", &g_win_bus, &g_win_cpu, &g_win_size) ||
        !fdt_translate(fdt_parent(n), &g_win_cpu)) {
        kprintf("pcie: no usable memory window in the device tree's `ranges`\n");
        return false;
    }
    if (!read_window(n, "dma-ranges", &in_pci, &in_cpu, &in_size)) {
        in_pci = 0; in_cpu = 0; in_size = 0x100000000ull;   /* 4 GiB at 0 */
        kprintf("pcie: no dma-ranges; assuming the first 4 GiB at bus address 0\n");
    }

    base = (volatile uint8_t *)(uintptr_t)vmm_map_mmio(phys, size ? size : 0x9310);
    if (!base) {
        kprintf("pcie: could not map the controller at %p\n", (void *)(uintptr_t)phys);
        return false;
    }
    kprintf("pcie: BCM2711 at %p, revision %x; window CPU %p = PCI %p (%d MiB)\n",
            (void *)(uintptr_t)phys, rd(MISC_REVISION),
            (void *)(uintptr_t)g_win_cpu, (void *)(uintptr_t)g_win_bus,
            (int)(g_win_size >> 20));

    /* 1. Reset the bridge and assert PERST# to the device; then release the
     *    bridge, leaving the device held. */
    wr(RGR1_SW_INIT_1, rd(RGR1_SW_INIT_1) | SW_INIT_1_INIT);
    wr(RGR1_SW_INIT_1, rd(RGR1_SW_INIT_1) | SW_INIT_1_PERST);
    timer_delay_ms(1);
    wr(RGR1_SW_INIT_1, rd(RGR1_SW_INIT_1) & ~SW_INIT_1_INIT);

    /* 2. SerDes out of IDDQ (its power-down) and give it time to settle. */
    wr(MISC_HARD_PCIE_HARD_DEBUG, rd(MISC_HARD_PCIE_HARD_DEBUG) & ~HARD_DEBUG_SERDES_IDDQ);
    timer_delay_ms(1);

    /* 3. SCB access on, config reads of nothing answer all-ones instead of an
     *    error (so a scan cannot fault), 128-byte bursts. */
    uint32_t ctrl = rd(MISC_MISC_CTRL);
    ctrl |= MISC_CTRL_SCB_ACCESS_EN | MISC_CTRL_CFG_READ_UR_MODE;
    ctrl = field(ctrl, MISC_CTRL_MAX_BURST_MASK, 0);
    wr(MISC_MISC_CTRL, ctrl);

    /* 4. Inbound: device DMA to memory, through RC_BAR2, sized as a power of
     *    two covering dma-ranges -- which is what makes a buffer's physical
     *    address the address the VL805 is given. */
    uint64_t bar2 = 1ull << (ilog2_64(in_size - 1) + 1);
    uint64_t bar2_off = in_pci - in_cpu;
    wr(MISC_RC_BAR2_CONFIG_LO, field((uint32_t)bar2_off, RC_BAR_SIZE_MASK, encode_ibar_size(bar2)));
    wr(MISC_RC_BAR2_CONFIG_HI, (uint32_t)(bar2_off >> 32));
    wr(MISC_MISC_CTRL, field(rd(MISC_MISC_CTRL), MISC_CTRL_SCB0_SIZE_MASK, ilog2_64(bar2) - 15));
    /* ...and the two windows nobody uses closed. */
    wr(MISC_RC_BAR1_CONFIG_LO, rd(MISC_RC_BAR1_CONFIG_LO) & ~RC_BAR_SIZE_MASK);
    wr(MISC_RC_BAR3_CONFIG_LO, rd(MISC_RC_BAR3_CONFIG_LO) & ~RC_BAR_SIZE_MASK);

    /* 5. Every interrupt the controller could raise, masked and cleared: the
     *    xHCI behind it is polled (docs/RPI4.md P5). */
    wr(MSI_INTR2_BASE + INTR2_MASK_SET, 0xFFFFFFFFu);
    wr(MSI_INTR2_BASE + INTR2_CLR, 0xFFFFFFFFu);

    /* 6. Class code: this is a PCI-to-PCI bridge, which the ROM does not say. */
    wr(RC_CFG_PRIV1_ID_VAL3, field(rd(RC_CFG_PRIV1_ID_VAL3), ID_VAL3_CLASS_CODE_MASK, 0x060400));

    /* 7. Release PERST# and wait for the link: up to 100 ms, as Linux does. */
    wr(RGR1_SW_INIT_1, rd(RGR1_SW_INIT_1) & ~SW_INIT_1_PERST);
    int waited = 0;
    while (!link_up() && waited < 100) { timer_delay_ms(5); waited += 5; }
    uint32_t st = rd(MISC_PCIE_STATUS);
    if (!link_up()) {
        kprintf("pcie: LINK DOWN after %d ms (status %x) -- no USB on this Pi\n",
                waited, st);
        return false;
    }
    if (!(st & STATUS_PORT_RC)) {
        kprintf("pcie: the controller is in endpoint mode (status %x) -- cannot use it\n", st);
        return false;
    }
    uint16_t lnksta = (uint16_t)(rd(BRCM_PCIE_CAP_REGS + (PCI_EXP_LNKSTA & ~3u)) >> 16);
    kprintf("pcie: link up after %d ms: gen %u x%u\n",
            waited, lnksta & 0xFu, (lnksta >> 4) & 0x3Fu);

    /* 8. Outbound: CPU window -> PCI addresses. Base and limit in megabytes,
     *    low 12 bits in BASE_LIMIT, the rest in the _HI registers. */
    uint64_t cpu_mb = g_win_cpu >> 20, lim_mb = (g_win_cpu + g_win_size - 1) >> 20;
    wr(MISC_CPU_2_PCIE_MEM_WIN0_LO, (uint32_t)g_win_bus);
    wr(MISC_CPU_2_PCIE_MEM_WIN0_HI, (uint32_t)(g_win_bus >> 32));
    uint32_t bl = rd(MISC_WIN0_BASE_LIMIT);
    bl = field(bl, WIN_BASE_MASK, (uint32_t)cpu_mb);
    bl = field(bl, WIN_LIMIT_MASK, (uint32_t)lim_mb);
    wr(MISC_WIN0_BASE_LIMIT, bl);
    wr(MISC_WIN0_BASE_HI,  field(rd(MISC_WIN0_BASE_HI),  WIN_HI_MASK, (uint32_t)(cpu_mb >> 12)));
    wr(MISC_WIN0_LIMIT_HI, field(rd(MISC_WIN0_LIMIT_HI), WIN_HI_MASK, (uint32_t)(lim_mb >> 12)));

    /* 9. Inbound data little-endian; CLKREQ# gating as Linux sets it. */
    wr(RC_CFG_VENDOR_SPECIFIC_REG1, field(rd(RC_CFG_VENDOR_SPECIFIC_REG1), REG1_ENDIAN_MODE_BAR2_MASK, 0));
    wr(MISC_HARD_PCIE_HARD_DEBUG, rd(MISC_HARD_PCIE_HARD_DEBUG) | HARD_DEBUG_CLKREQ_DEBUG_EN);

    /* 10. The root port as a bridge: bus 0 -> 1, forwarding the whole memory
     *     window, decoding memory and mastering (a device behind a bridge can
     *     only DMA if the bridge masters). Prefetchable window closed. With
     *     these set, pci_bridge_configure() finds it configured and leaves it. */
    wr(PCI_BUS_NUMBERS, (rd(PCI_BUS_NUMBERS) & 0xFF000000u) | (1u << 16) | (1u << 8) | 0u);
    uint32_t mb = (uint32_t)(g_win_bus >> 16) & 0xFFF0u;
    uint32_t ml = (uint32_t)((g_win_bus + g_win_size - 1) >> 16) & 0xFFF0u;
    wr(PCI_MEM_BASE_LIMIT, mb | (ml << 16));
    wr(PCI_PREF_BASE_LIMIT, 0x0000FFF0u);                 /* base > limit: off */
    wr(PCI_COMMAND, (rd(PCI_COMMAND) & 0xFFFF0000u) | 0x0006u);

    g_up = true;
    return true;
}

bool brcm_pcie_active(void) { return g_up; }

/* The address of config dword `offset`, or 0 for nothing there. Takes the
 * lock for bus > 0 (index-then-data is two accesses); releases in _done. */
static volatile uint32_t *cfg_word(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
    if (!g_up || dev != 0 || bus > 1)
        return 0;
    if (bus == 0)
        return fn == 0 ? (volatile uint32_t *)(base + (offset & 0xFCu)) : 0;
    spin_lock(&g_cfg_lock);
    wr(EXT_CFG_INDEX, ((uint32_t)bus << 20) | ((uint32_t)dev << 15) | ((uint32_t)fn << 12));
    return (volatile uint32_t *)(base + EXT_CFG_DATA + (offset & 0xFCu));
}

uint32_t brcm_pcie_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset) {
    volatile uint32_t *p = cfg_word(bus, dev, fn, offset);
    if (!p) return 0xFFFFFFFFu;
    uint32_t v = *p;
    if (bus > 0) spin_unlock(&g_cfg_lock);
    return v;
}

void brcm_pcie_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset, uint32_t v) {
    volatile uint32_t *p = cfg_word(bus, dev, fn, offset);
    if (!p) return;
    *p = v;
    if (bus > 0) spin_unlock(&g_cfg_lock);
}

uint64_t brcm_pcie_bus_to_cpu(uint64_t a) {
    return (g_up && a >= g_win_bus && a - g_win_bus < g_win_size)
           ? a - g_win_bus + g_win_cpu : a;
}

uint64_t brcm_pcie_cpu_to_bus(uint64_t a) {
    return (g_up && a >= g_win_cpu && a - g_win_cpu < g_win_size)
           ? a - g_win_cpu + g_win_bus : a;
}

void brcm_pcie_assign_resources(void) {
    if (!g_up)
        return;
    /* BARs go in the window from the tree, as CPU addresses: pci.c converts
     * to bus addresses when it programs them (arch_pci_cpu_to_bus). */
    pci_assign_resources(g_win_cpu, g_win_size);

    /* THE VL805'S FIRMWARE. On the newer Pi 4 boards it has no EEPROM of its
     * own: after a PCIe reset -- which step 1 of brcm_pcie_init() was -- the
     * VideoCore must load it, and must be told to. Exactly as Linux's
     * rpi_firmware_init_vl805(): read the firmware-version register at config
     * offset 0x50; zero means nothing is running, so ask. Before anything
     * touches the xHCI. */
    for (uint32_t i = 0; i < pci_devices_count(); i++) {
        const struct pci_device *d = pci_get_device(i);
        if (!d || d->bus != 1 || d->class_code != 0x0C || d->subclass != 0x03)
            continue;
        uint32_t before = pci_read32(d->bus, d->device, d->function, 0x50);
        bool asked = false, ok = false;
        if (before == 0) {
            uint32_t addr = ((uint32_t)d->bus << 20) | ((uint32_t)d->device << 15) |
                            ((uint32_t)d->function << 12);
            asked = true;
            ok = bcm_mbox_notify_xhci_reset(addr);
            timer_delay_ms(1);                           /* let it start */
        }
        uint32_t after = pci_read32(d->bus, d->device, d->function, 0x50);
        kprintf("pcie: USB controller %x:%x at 1:%d.%d; firmware version %x%s%s\n",
                d->vendor_id, d->device_id, d->device, d->function, after,
                asked ? (ok ? " (loaded by the VideoCore just now)"
                            : " (the VideoCore did NOT acknowledge the load request)")
                      : " (already running)",
                after == 0 ? " -- STILL NO FIRMWARE: USB will not work" : "");

        /* The reload resets the controller: whatever of its configuration
         * did not survive is put back. pci_assign_resources() only places
         * BARs that read as unplaced, so this is idempotent. */
        pci_assign_resources(g_win_cpu, g_win_size);
        uint16_t cmd = pci_read16(d->bus, d->device, d->function, 0x04);
        pci_write16(d->bus, d->device, d->function, 0x04, (uint16_t)(cmd | 0x0006u));
    }
}
