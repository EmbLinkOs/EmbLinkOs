/* Universal Flash Storage: SCSI commands inside UPIU packets inside transfer
 * request descriptors, over a serial link that has to be started first. */
#ifndef _EMBK_UFS_H_
#define _EMBK_UFS_H_
#include <stdint.h>
#include "include/types.h"
bool     ufs_init(void);
bool     ufs_present(void);
uint32_t ufs_lun_count(void);
#endif
