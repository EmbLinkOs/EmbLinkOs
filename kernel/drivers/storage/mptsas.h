/* LSI Fusion-MPT: message passing through two hardware FIFOs, with two kinds
 * of answer told apart by one bit. See the .c. */
#ifndef _EMBK_MPTSAS_H_
#define _EMBK_MPTSAS_H_
#include <stdint.h>
#include "include/types.h"
bool     mptsas_init(void);
bool     mptsas_present(void);
uint32_t mptsas_target_count(void);
#endif
