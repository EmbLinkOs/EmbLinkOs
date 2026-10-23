#ifndef EMBLINK_DISPLAY_ABI_H
#define EMBLINK_DISPLAY_ABI_H
#include <stdint.h>
#define EMBK_DISPLAY_VERSION 1
#define EMBK_DISPLAY_BACKLIGHT 1u
#define EMBK_DISPLAY_BUSY 2u
#define EMBK_DISPLAY_KNOWN 4u
#define EMBK_DISPLAY_NATIVE 8u
struct embk_display_info {
    uint32_t version,flags,percent,level_count;
    uint32_t levels[32];
    uint64_t sampled_ms;
    int32_t last_error;
    uint32_t reserved;
};
#endif
