#ifndef EMBLINK_ACPI_DISPLAY_H
#define EMBLINK_ACPI_DISPLAY_H
#include "../../user/lib/embk_display.h"
void acpi_display_init(void);
void acpi_display_refresh(void);
int acpi_display_snapshot(struct embk_display_info *);
int acpi_display_request(unsigned percent);
#endif
