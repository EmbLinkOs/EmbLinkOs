#ifndef ARCH_AARCH64_PCIE_BRCMSTB_H
#define ARCH_AARCH64_PCIE_BRCMSTB_H

#include <stdint.h>
#include "include/types.h"

/* The Raspberry Pi 4's PCIe host bridge -- see pcie_brcmstb.c and
 * docs/RPI4.md P5. The functions below back the arch_pci_* hooks in
 * pci_ecam.c when the machine has this controller instead of an ECAM. */

/* Reset the controller, train the link, open its windows. False (with the
 * reason printed) if there is no such controller or the link did not come up. */
bool     brcm_pcie_init(void);
bool     brcm_pcie_active(void);

uint32_t brcm_pcie_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);
void     brcm_pcie_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset,
                               uint32_t value);
uint64_t brcm_pcie_bus_to_cpu(uint64_t bus_addr);
uint64_t brcm_pcie_cpu_to_bus(uint64_t cpu_addr);

/* After pci_init(): place BARs in the window, then have the firmware load the
 * VL805's firmware. */
void     brcm_pcie_assign_resources(void);

#endif
