#ifndef EMBLINK_DISPLAY_CONTROL_H
#define EMBLINK_DISPLAY_CONTROL_H
#include "../../user/lib/embk_monitor.h"
int monitor_control(unsigned,void *,unsigned);
void monitor_tick(void);
/* The refresh of the primary display's current mode, in millihertz, as its
 * EDID gives it; 0 when nothing says. For the compositor's frame clock: never
 * call it holding the compositor's lock (it takes it to describe the screen). */
uint32_t display_refresh_mhz(void);
#endif
