#ifndef EMBK_MONITOR_H
#define EMBK_MONITOR_H
#include <stdint.h>
#define EMBK_MONITOR_VERSION 1u
#define EMBK_MONITOR_MAX_MODES 24u
#define EMBK_MONITOR_CONNECTED 1u
#define EMBK_MONITOR_MODESET 2u
#define EMBK_MONITOR_EDID 4u
#define EMBK_MONITOR_OWN_TRIAL 8u
#define EMBK_MONITOR_PREFERRED 1u
#define EMBK_MONITOR_CURRENT 2u
enum { EMBK_MONITOR_GET=0, EMBK_MONITOR_APPLY, EMBK_MONITOR_KEEP, EMBK_MONITOR_REVERT };
enum { EMBK_MONITOR_IDLE=0, EMBK_MONITOR_QUEUED, EMBK_MONITOR_APPLYING, EMBK_MONITOR_TRIAL, EMBK_MONITOR_REVERTING, EMBK_MONITOR_ERROR };
struct embk_monitor_mode {uint32_t width,height,refresh_millihz,flags;};
struct embk_monitor_info {
    uint32_t version,flags,generation,phase;
    uint32_t width,height,mode_count,remaining_ms;
    uint64_t token;
    int32_t last_error;uint32_t width_mm,height_mm;
    char name[64],manufacturer[4],serial[24],driver[64];
    struct embk_monitor_mode modes[EMBK_MONITOR_MAX_MODES];
};
struct embk_monitor_request {uint32_t version,generation,width,height;uint64_t token;};
#endif
