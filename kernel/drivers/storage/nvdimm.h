/* Persistent memory that is not a device: memory in a slot, and an ACPI table
 * that says it is different from ordinary RAM. The type GUID is the only thing
 * that distinguishes it. See the .c. */
#ifndef _EMBK_NVDIMM_H_
#define _EMBK_NVDIMM_H_
#include <stdint.h>
#include "include/types.h"
bool     nvdimm_init(void);
uint32_t nvdimm_count(void);
uint64_t nvdimm_bytes(uint32_t i);
#endif
