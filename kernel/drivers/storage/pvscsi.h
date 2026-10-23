/* VMware's paravirtual SCSI controller: two rings and a kick register, with
 * every pointer expressed as a page frame number. See the .c. */
#ifndef _EMBK_PVSCSI_H_
#define _EMBK_PVSCSI_H_
#include <stdint.h>
#include "include/types.h"
bool     pvscsi_init(void);
bool     pvscsi_present(void);
uint32_t pvscsi_target_count(void);
#endif
