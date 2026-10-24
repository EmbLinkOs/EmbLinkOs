#ifndef __EMBLINK_UI_THEME_H__
#define __EMBLINK_UI_THEME_H__

/* Contour semantic tokens. Surface families, readable text, shared geometry,
 * typography, materials and motion are coordinated here. Applications use
 * semantic roles; user preferences are applied by user/lib/appearance.h. */

#include <stdint.h>
#include "scene.h"   /* struct color */

struct ui_shadow_spec { float dx, dy, blur; struct color color; };

struct ui_theme {
    /* --- surfaces & lines --- */
    struct color sidebar, chrome;
    float control_height, toolbar_height, control_py, control_px;
    float material_alpha, blur, motion_ms;
    int reduced_motion, icon_theme, decoration;
    struct color bg;             /* app background */
    struct color surface;        /* cards, panels */
    struct color surface_alt;    /* insets, subtle fills (input wells, hovers) */
    struct color border;         /* hairline dividers / card edges */
    struct color border_strong;  /* emphasised edges, focus rings */

    /* --- text (three-step hierarchy) --- */
    struct color text;           /* primary */
    struct color text_secondary; /* labels, supporting copy */
    struct color text_tertiary;  /* captions, disabled, metadata */

    /* --- accent (spend boldness here only) --- */
    struct color accent;
    struct color accent_hover;
    struct color accent_soft;    /* tinted background for accent surfaces/badges */
    /* Behind SELECTED TEXT, and a stronger tint than accent_soft on purpose: a
     * soft plate says "this row is the current one" and may whisper, but a text
     * selection is the thing the next keystroke is about to destroy. It has to
     * be unmistakable, and at accent_soft's 18% it was not. */
    struct color selection;
    struct color on_accent;      /* text/icon on an accent fill */

    /* --- semantic status (separate from the accent) --- */
    struct color success, warning, danger;

    /* --- radii --- */
    float radius_sm, radius_md, radius_lg, radius_pill;

    /* --- spacing scale (8px grid; sp1 is the 4px half-step) --- */
    float sp1, sp2, sp3, sp4, sp5, sp6, sp7;   /* 4 8 12 16 24 32 48 */

    /* --- type scale (px) --- */
    float text_caption, text_body, text_body_lg, text_title, text_heading;

    /* --- elevation --- */
    struct ui_shadow_spec shadow_sm, shadow_md, shadow_lg;

    /* --- fonts (set by the app after loading its .ttf files) --- */
    uint32_t font_regular, font_bold;
};

struct ui_appearance {
    int surface_style, density, contrast, transparency, blur, reduce_motion;
    int radius, shadows, icon_theme, decoration, text_scale;
    uint32_t surface_rgb;
};
void ui_theme_configure(const struct ui_appearance *appearance);

/* The active theme. Never NULL. */
const struct ui_theme *ui_theme(void);

/* Swap light/dark (preserves the font handles). */
void ui_theme_use_dark(bool dark);

/* Register the loaded font handles the kit uses for regular vs. bold text. */
void ui_theme_set_fonts(uint32_t regular, uint32_t bold);
/* Re-tint the accent (and its derived hover/soft/on-accent tones) at runtime. */
void ui_theme_set_accent(struct color c);
/* Interface size: scales the whole type scale, and so everything measured from
 * it. 1.0 = default; clamped to [0.80, 1.30]. */
void ui_theme_set_scale(float s);

#endif /* __EMBLINK_UI_THEME_H__ */
