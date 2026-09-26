#include "arch/aarch64/drivers/sdhci_dt.h"
#include "arch/aarch64/drivers/bcm_mbox.h"
#include "arch/aarch64/boot/fdt.h"
#include "drivers/storage/sdhci.h"
#include "include/kprintf.h"

/* SD host controllers that are not on PCI -- docs/RPI4.md P3.
 *
 * A Raspberry Pi 4's SD slot, the one it boots from, is EMMC2: an SDHCI at a
 * fixed address, described only by the device tree. The controller itself is
 * the generic one kernel/drivers/storage/sdhci.c already drives; what the
 * tree adds is where it is, and two facts about this board:
 *
 *   * It is Broadcom iProc IP, which implements only 32-bit register
 *     accesses (Linux's sdhci-iproc says so and works around it the same way).
 *   * `broken-cd`: the card-detect line is not wired, so "no card" from the
 *     controller means nothing. The Pi booted from this card; ask it.
 *
 * Its base clock belongs to the firmware, like every clock on a Pi, and is
 * asked for over the mailbox -- the capabilities register may not say.
 *
 * THE OLDER CONTROLLER, brcm,bcm2835-sdhci at 0x7e300000, is here too: the
 * same generic SDHCI, the same 32-bit-only rule, and the SD controller of the
 * earlier Pis. On a Pi 4 its SD-card node is `disabled` and the node that IS
 * enabled at that address (`mmcnr`) is the Wi-Fi chip's SDIO link -- which
 * this driver must never treat as a disk, and recognises by the child node an
 * SDIO function has (wifi@1). QEMU's raspi4b, unlike the board, wires its SD
 * card to THIS controller and leaves EMMC2 empty; test-rpi4-boot boots it with
 * the SD-card node enabled to match (docs/RPI4.md P3). */

static const struct {
    const char *compat;
    uint32_t    flags;
    uint32_t    fw_clock;        /* BCM mailbox clock ID, 0 = none */
    const char *where;
} hosts[] = {
    { "brcm,bcm2711-emmc2", SDHCI_32BIT_ONLY, BCM_CLOCK_EMMC2, "the SD slot (EMMC2)" },
    { "brcm,bcm2835-sdhci", SDHCI_32BIT_ONLY, BCM_CLOCK_EMMC,  "the Arasan SDHCI (0x7e300000)" },
};

/* An SDIO host: a controller whose node has a child describing the function
 * behind it (the Pi's Wi-Fi, wifi@1). Not storage, however it is wired. */
static bool is_sdio_host(fdt_node_t n) {
    for (fdt_node_t c = fdt_first_child(n); c != FDT_NONE; c = fdt_next_sibling(c)) {
        uint32_t len = 0;
        if (fdt_prop(c, "compatible", &len))
            return true;
    }
    return false;
}

void sdhci_dt_init(void) {
    for (unsigned i = 0; i < sizeof hosts / sizeof hosts[0]; i++)
    for (fdt_node_t n = fdt_find_compatible(hosts[i].compat); n != FDT_NONE;
         n = fdt_find_compatible_after(hosts[i].compat, n)) {
        uint32_t len = 0;
        const char *status = (const char *)fdt_prop(n, "status", &len);
        if (status && len && !fdt_prop_has_string(n, "status", "okay") &&
            !fdt_prop_has_string(n, "status", "ok"))
            continue;                                   /* "disabled" */
        if (is_sdio_host(n))
            continue;                                   /* the Wi-Fi, not a card */

        uint64_t phys = 0, size = 0;
        if (!fdt_reg(n, 0, &phys, &size)) {
            kprintf("sdhci: %s has no usable address in the device tree\n", hosts[i].compat);
            continue;
        }

        uint32_t flags = hosts[i].flags;
        if (fdt_prop(n, "broken-cd", &len))
            flags |= SDHCI_BROKEN_CD;

        uint32_t hz = hosts[i].fw_clock ? bcm_mbox_clock_rate(hosts[i].fw_clock) : 0;
        kprintf("sdhci: %s at %p%s, firmware says base clock %u kHz\n",
                hosts[i].compat, (void *)(uintptr_t)phys,
                (flags & SDHCI_BROKEN_CD) ? " (no card-detect)" : "", hz / 1000);
        sdhci_attach_mmio(phys, size, flags, hz / 1000, hosts[i].where);
    }
}
