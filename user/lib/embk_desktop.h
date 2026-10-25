#ifndef EMBK_DESKTOP_ABI_H
#define EMBK_DESKTOP_ABI_H
#include <stdint.h>
#define EMBK_DESKTOP_VERSION 1u
#define EMBK_DESKTOP_GET 0u
#define EMBK_DESKTOP_CONFIGURE 1u /* only the process owning the desktop layer */
#define EMBK_DESKTOP_SWITCH 2u
#define EMBK_DESKTOP_MOVE_FRONT 3u
#define EMBK_DESKTOP_REDUCED_MOTION 1u
#define EMBK_DESKTOP_SNAP 2u
#define EMBK_DESKTOP_WRAP 4u
struct embk_desktop {
    uint32_t version, flags;
    uint32_t left,top,right,bottom;
    uint32_t background,text,accent,danger,secondary;
    uint32_t radius,shadows,decoration,cursor_style,cursor_size;
    uint32_t workspace_count,workspace;
    uint32_t shortcut_modifier,blur,opacity;
};
#endif
