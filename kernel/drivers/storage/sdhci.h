/* The SD/eMMC host controller -- the card reader in the side of a laptop, and
 * on many ARM boards the thing the machine boots from. See the .c for why the
 * card's addressing mode is the one detail that has to be right. */
#ifndef _EMBK_SDHCI_H_
#define _EMBK_SDHCI_H_
#include <stdint.h>
#include "include/types.h"
void     sdhci_init(void);
uint32_t sdhci_host_count(void);
bool     sdhci_card_present(uint32_t idx);
uint64_t sdhci_blocks(uint32_t idx);
#endif
