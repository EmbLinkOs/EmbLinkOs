/* Giving memory back to the host. The pages handed over must not be touched
 * again until they are deflated -- nothing enforces that but this driver. */
#ifndef _EMBK_VIRTIO_BALLOON_H_
#define _EMBK_VIRTIO_BALLOON_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_balloon_init(void);
bool     virtio_balloon_present(void);
uint32_t virtio_balloon_inflate(uint32_t pages);
uint32_t virtio_balloon_deflate(uint32_t pages);
uint32_t virtio_balloon_held(void);
uint32_t virtio_balloon_target(void);
#endif
