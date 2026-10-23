/* virtio-scsi: a controller with real SCSI targets, unlike virtio-blk's one
 * bare disk. Commands come from kernel/block/scsi.c. See the .c. */
#ifndef _EMBK_VIRTIO_SCSI_H_
#define _EMBK_VIRTIO_SCSI_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_scsi_init(void);
bool     virtio_scsi_present(void);
uint32_t virtio_scsi_target_count(void);
#endif
