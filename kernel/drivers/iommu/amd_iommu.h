/* AMD-Vi: one flat device table indexed by requester id, and invalidations
 * posted as COMMANDS into a buffer rather than written to a register -- so
 * the command buffer has to be running before the IOMMU is. See the .c. */
#ifndef _EMBK_AMD_IOMMU_H_
#define _EMBK_AMD_IOMMU_H_
#include <stdint.h>
#include "include/types.h"
bool     amd_iommu_init(void);
bool     amd_iommu_present(void);
uint32_t amd_iommu_devices(void);
#endif
