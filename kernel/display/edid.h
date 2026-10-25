#ifndef EMBLINK_EDID_H
#define EMBLINK_EDID_H
#include <stddef.h>
#include <stdint.h>
struct display_identity {
    char name[64],manufacturer[4],serial[24];
    uint32_t width_mm,height_mm,preferred_width,preferred_height,refresh_millihz;
};
/* Accept only a complete, checksum-valid base EDID with bounded text. */
int display_edid_parse(const uint8_t *,size_t,struct display_identity *);
struct embk_monitor_info;
void display_identity_describe(const struct display_identity *,struct embk_monitor_info *);
#endif
