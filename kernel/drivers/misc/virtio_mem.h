/* Memory added a block at a time, inside a region reserved at start-up. The
 * host's size is a REQUEST, not a command -- and nothing here unplugs, because
 * this allocator cannot vacate a range that is in use. See the .c. */
#ifndef _EMBK_VIRTIO_MEM_H_
#define _EMBK_VIRTIO_MEM_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_mem_init(void);
bool     virtio_mem_present(void);
uint64_t virtio_mem_sync(void);
uint64_t virtio_mem_plugged(void);
uint64_t virtio_mem_region(void);
#endif
