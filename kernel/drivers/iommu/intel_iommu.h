/* VT-d: the IOMMU a real x86 machine has. Root table by bus, context table by
 * device, then a page table -- and caches that are not coherent with memory,
 * so every structural change needs an explicit invalidation. See the .c. */
#ifndef _EMBK_INTEL_IOMMU_H_
#define _EMBK_INTEL_IOMMU_H_
#include <stdint.h>
#include "include/types.h"
bool     intel_iommu_init(void);
bool     intel_iommu_present(void);
uint32_t intel_iommu_units(void);
uint32_t intel_iommu_devices(void);
#endif
