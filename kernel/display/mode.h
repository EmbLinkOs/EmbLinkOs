/* mode.h -- one display timing, the form every display driver and the EDID
 * parser exchange. Same fields as a VESA/CTA detailed timing. */
#ifndef EMBLINK_DISPLAY_MODE_H
#define EMBLINK_DISPLAY_MODE_H
#include <stdint.h>

#define DISPLAY_MODE_HSYNC_POSITIVE 0x01u
#define DISPLAY_MODE_VSYNC_POSITIVE 0x02u
#define DISPLAY_MODE_INTERLACED     0x04u
#define DISPLAY_MODE_PREFERRED      0x08u

struct display_mode {
    uint32_t clock_khz;
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    uint8_t  flags;
    uint8_t  vic;                  /* CTA-861 video identification code, 0 if none */
};

/* Refresh in millihertz, 0 for a timing that cannot be one. */
static inline uint32_t display_mode_refresh_mhz(const struct display_mode *m)
{
    uint64_t pixels = (uint64_t)m->htotal * m->vtotal;
    return pixels ? (uint32_t)(((uint64_t)m->clock_khz * 1000000u + pixels / 2) / pixels) : 0;
}

/* A timing whose fields are in order and inside what any driver here drives. */
static inline int display_mode_sane(const struct display_mode *m)
{
    return m->clock_khz >= 10000 && m->clock_khz <= 600000 &&
           m->hdisplay >= 320 && m->hdisplay <= 8192 && m->vdisplay >= 200 && m->vdisplay <= 4320 &&
           m->hdisplay <= m->hsync_start && m->hsync_start < m->hsync_end && m->hsync_end <= m->htotal &&
           m->vdisplay <= m->vsync_start && m->vsync_start < m->vsync_end && m->vsync_end <= m->vtotal &&
           m->hdisplay < m->htotal && m->vdisplay < m->vtotal &&
           !(m->flags & DISPLAY_MODE_INTERLACED);
}

#endif
