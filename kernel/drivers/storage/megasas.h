/* LSI MegaRAID SAS: a firmware with a mailbox, not a register interface. What
 * the driver sees are LOGICAL drives the controller assembled, never the
 * physical ones behind them. See the .c. */
#ifndef _EMBK_MEGASAS_H_
#define _EMBK_MEGASAS_H_
#include <stdint.h>
#include "include/types.h"
bool     megasas_init(void);
bool     megasas_present(void);
uint32_t megasas_ld_count(void);
#endif
