/* Memory that survives a reboot: the device puts a persistent region in the
 * guest's physical address space and the virtqueue is only for making writes
 * durable. See the .c on why a store is not a commit. */
#ifndef _EMBK_VIRTIO_PMEM_H_
#define _EMBK_VIRTIO_PMEM_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_pmem_init(void);
bool     virtio_pmem_present(void);
uint64_t virtio_pmem_size(void);
uint64_t virtio_pmem_flushes(void);
int      virtio_pmem_flush(void);
#endif
