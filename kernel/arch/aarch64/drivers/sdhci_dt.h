#ifndef ARCH_AARCH64_SDHCI_DT_H
#define ARCH_AARCH64_SDHCI_DT_H

/* Attach every SD host controller the device tree describes that is not on a
 * PCI bus -- a Raspberry Pi 4's SD slot. See sdhci_dt.c. */
void sdhci_dt_init(void);

#endif
