/* A processor or a memory stick that was not there a moment ago. The firmware
 * has been declaring both in the DSDT since the AML interpreter was written;
 * this is the code that listens. Polled, not interrupt-driven -- see the .c. */
#ifndef _EMBK_ACPI_HOTPLUG_H_
#define _EMBK_ACPI_HOTPLUG_H_
#include <stdint.h>
#include "include/types.h"
void     acpi_hotplug_init(void);
void     acpi_hotplug_poll(void);
bool     acpi_hotplug_available(void);
uint32_t acpi_hotplug_cpus_added(void);
uint32_t acpi_hotplug_memory_added(void);
uint64_t acpi_hotplug_memory_bytes(void);
#endif
