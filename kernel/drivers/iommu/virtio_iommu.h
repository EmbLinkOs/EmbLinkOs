/* The device that decides what a device is allowed to reach. The one driver
 * here that can stop the machine by working: an attached endpoint with an
 * empty domain can no longer reach its own descriptors. See the .c. */
#ifndef _EMBK_VIRTIO_IOMMU_H_
#define _EMBK_VIRTIO_IOMMU_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_iommu_init(void);
bool     virtio_iommu_present(void);
uint32_t virtio_iommu_endpoints(void);
uint64_t virtio_iommu_mapped(void);
#endif
