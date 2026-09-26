/* The SD/eMMC host controller -- the card reader in the side of a laptop, and
 * on many ARM boards the thing the machine boots from. See the .c for why the
 * card's addressing mode is the one detail that has to be right. */
#ifndef _EMBK_SDHCI_H_
#define _EMBK_SDHCI_H_
#include <stdint.h>
#include "include/types.h"
void     sdhci_init(void);           /* every SD host controller on PCI */

/* A controller at a known physical address -- a board's SD slot, found in its
 * device tree. `base_khz` is the controller's base clock when the platform
 * knows it (0 if not; the capabilities register wins when it says). `where`
 * names it in the log and must outlive the call. */
#define SDHCI_32BIT_ONLY  (1u << 0)   /* no 8/16-bit register accesses (iProc) */
#define SDHCI_BROKEN_CD   (1u << 1)   /* the card-detect bit means nothing     */
int      sdhci_attach_mmio(uint64_t phys, uint64_t size, uint32_t flags,
                           uint32_t base_khz, const char *where);
uint32_t sdhci_host_count(void);
bool     sdhci_card_present(uint32_t idx);
uint64_t sdhci_blocks(uint32_t idx);
#endif
