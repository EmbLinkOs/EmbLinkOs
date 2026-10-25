#ifndef EMBLINK_INTEL_DISPLAY_H
#define EMBLINK_INTEL_DISPLAY_H
#include "drivers/video/gpu.h"
const struct gpu_driver *intel_display_probe(void);
int intel_backlight_get(unsigned *percent);
int intel_backlight_set(unsigned percent);
#endif
