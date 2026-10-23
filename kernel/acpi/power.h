#ifndef EMBLINK_POWER_H
#define EMBLINK_POWER_H
#include "../../user/lib/embk_power.h"
void acpi_power_init(void);
void acpi_power_tick(void);
int acpi_power_snapshot(struct embk_power_info *);
int acpi_power_request(unsigned action);
/* Firmware namespace is fully available after runtime initialization. */
int acpi_runtime_ready(void);
#endif
