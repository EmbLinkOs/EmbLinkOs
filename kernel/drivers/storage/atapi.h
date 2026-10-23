/* SCSI commands down an IDE cable: the optical drive. 2048-byte blocks, and
 * an empty drive is not a broken one. See the .c. */
#ifndef _EMBK_ATAPI_H_
#define _EMBK_ATAPI_H_
#include <stdint.h>
#include "include/types.h"
void        atapi_init(void);
uint32_t    atapi_drive_count(void);
const char *atapi_drive_name(uint32_t i);
uint64_t    atapi_drive_blocks(uint32_t i);
#endif
