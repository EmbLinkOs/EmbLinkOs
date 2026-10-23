/* Entropy from the host, for a guest whose CPU offers none. See the .c. */
#ifndef _EMBK_VIRTIO_RNG_H_
#define _EMBK_VIRTIO_RNG_H_

#include <stdint.h>
#include "include/types.h"

/* Find and bring up the device. False means there is none, which is normal. */
bool virtio_rng_init(void);

/* Ask for `len` bytes (capped at 64) and fold them into the kernel CSPRNG.
 * Returns how many arrived -- a short return is legal and is the device
 * saying its own source is running dry, not an error. */
uint32_t virtio_rng_harvest(uint32_t len);

/* As above, with a copy of the bytes for a caller that wants to judge them.
 * `out` may be NULL. See the .c on why the copy exists. */
uint32_t virtio_rng_read(void *out, uint32_t len);

bool     virtio_rng_present(void);
uint64_t virtio_rng_bytes(void);

#endif
