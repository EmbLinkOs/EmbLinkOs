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

/* ---- several monitors --------------------------------------------------------
 * EMBK_MONITOR_LAYOUT_GET fills a struct embk_display_layout: every output the
 * adapter has, connected or not, and where each enabled one sits on the
 * desktop (one surface, the bounding box of the enabled outputs). Anyone may
 * read it; an app places and maximizes its windows on the output they are on.
 * EMBK_MONITOR_LAYOUT_APPLY takes a struct embk_layout_request and starts the
 * same trial a resolution change does: KEEP or REVERT it with the token. */
#define EMBK_MONITOR_LAYOUT_GET   4u
#define EMBK_MONITOR_LAYOUT_APPLY 5u
#define EMBK_DISPLAY_MAX_OUTPUTS  4u
#define EMBK_DISPLAY_LAYOUT_VERSION 1u
#define EMBK_OUTPUT_CONNECTED 1u
#define EMBK_OUTPUT_ENABLED   2u
#define EMBK_OUTPUT_PRIMARY   4u
#define EMBK_OUTPUT_EDID      8u
#define EMBK_OUTPUT_BUILTIN  16u
#define EMBK_LAYOUT_MIRROR    1u     /* every enabled output shows the same picture */
#define EMBK_LAYOUT_MULTI     2u     /* the adapter drives more than one output */
struct embk_output {
    uint32_t flags;
    int32_t  x, y;                   /* on the desktop */
    uint32_t width, height, refresh_millihz;
    uint32_t width_mm, height_mm, mode_count;
    char     name[16], model[32], manufacturer[4];
    struct embk_monitor_mode modes[16];
};
struct embk_display_layout {
    uint32_t version, flags, count, generation;
    uint32_t desktop_width, desktop_height;
    uint32_t phase, remaining_ms;    /* the trial, as in embk_monitor_info */
    uint64_t token;
    int32_t  last_error;
    uint32_t max_enabled;            /* outputs that can be in use at once; 0: all of them */
    struct embk_output outputs[EMBK_DISPLAY_MAX_OUTPUTS];
};
struct embk_layout_request {
    uint32_t version, generation, flags, count;
    struct { uint32_t enabled, primary, width, height; int32_t x, y; } out[EMBK_DISPLAY_MAX_OUTPUTS];
};
#endif
