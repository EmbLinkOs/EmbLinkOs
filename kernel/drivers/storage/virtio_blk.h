#ifndef _VIRTIO_BLK_H_
#define _VIRTIO_BLK_H_

#include "include/types.h"

/* Find a virtio-blk device on the PCI bus, bring it up and register it with
 * the block layer. Returns false if there is none.
 *
 * Shared, not architecture-specific: virtio is a bus protocol. It exists
 * because QEMU `virt` has neither ATA nor AHCI, so this is the aarch64 port's
 * only path to a disk -- but x86 can use it too by attaching a
 * virtio-blk-pci device. */
bool virtio_blk_init(void);

#endif /* _VIRTIO_BLK_H_ */
