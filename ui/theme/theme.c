/* ui/theme/theme.c -- the two curated EmbLink themes (see theme.h). */

#include "theme.h"

#define C(r,g,b)     ((struct color){ (r)/255.0f, (g)/255.0f, (b)/255.0f, 1.0f })
#define CA(r,g,b,a)  ((struct color){ (r)/255.0f, (g)/255.0f, (b)/255.0f, (a) })

static bool  g_dark_pref = true;
static float g_ui_scale  = 1.0f;   /* the user's interface size, 1.0 = default */

static struct ui_theme light_theme(void) {
    struct ui_theme t = {0};
    t.bg            = C(251,251,252);
    t.surface       = C(255,255,255);
    t.surface_alt   = C(244,245,247);
    t.border        = C(230,232,236);
    t.border_strong = C(210,214,221);
    t.text          = C(23, 24, 28);
    t.text_secondary= C(91, 97,110);
    t.text_tertiary = C(146,151,162);
    t.accent        = C(84, 87,229);   /* #5457E5 */
    t.accent_hover  = C(74, 76,209);
    t.accent_soft   = C(238,239,254);
    t.on_accent     = C(255,255,255);
    t.success       = C(22,163, 74);
    t.warning       = C(217,119,  6);
    t.danger        = C(229, 72, 77);
    t.shadow_sm = (struct ui_shadow_spec){ 0, 1,  3, CA(15,18,32,0.07f) };
    t.shadow_md = (struct ui_shadow_spec){ 0, 6, 18, CA(15,18,32,0.10f) };
    t.shadow_lg = (struct ui_shadow_spec){ 0,18, 46, CA(15,18,32,0.16f) };
    return t;
}

static struct ui_theme dark_theme(void) {
    struct ui_theme t = {0};
    t.bg            = C(12, 13, 16);
    t.surface       = C(22, 24, 29);
    t.surface_alt   = C(29, 32, 39);
    t.border        = C(40, 44, 52);
    t.border_strong = C(54, 59, 69);
    t.text          = C(236,237,241);
    t.text_secondary= C(155,161,173);
    t.text_tertiary = C(106,113,128);
    t.accent        = C(124,130,255);  /* #7C82FF */
    t.accent_hover  = C(144,152,255);
    t.accent_soft   = C(30, 33, 64);
    t.on_accent     = C(255,255,255);
    t.success       = C(63,184,107);
    t.warning       = C(224,145, 58);
    t.danger        = C(242, 85, 90);
    t.shadow_sm = (struct ui_shadow_spec){ 0, 1,  3, CA(0,0,0,0.30f) };
    t.shadow_md = (struct ui_shadow_spec){ 0, 8, 22, CA(0,0,0,0.42f) };
    t.shadow_lg = (struct ui_shadow_spec){ 0,20, 52, CA(0,0,0,0.55f) };
    return t;
}

/* Scale + geometry are theme-independent (light/dark share them). */
static void apply_metrics(struct ui_theme *t) {
    t->radius_sm = 6;  t->radius_md = 9;  t->radius_lg = 14;  t->radius_pill = 999;
    t->sp1 = 4; t->sp2 = 8; t->sp3 = 12; t->sp4 = 16; t->sp5 = 24; t->sp6 = 32; t->sp7 = 48;
    /* The type scale, multiplied by the user's interface size. Everything the
     * UI measures is derived from these -- including the Terminal's character
     * grid -- so scaling here scales the whole system rather than one app's
     * idea of "bigger text". */
    t->text_caption = 12.5f * g_ui_scale; t->text_body = 14.0f * g_ui_scale;
    t->text_body_lg = 15.5f * g_ui_scale;
    t->text_title = 19.0f * g_ui_scale; t->text_heading = 26.0f * g_ui_scale;
}

static struct ui_theme g_current;
static bool g_init;
static uint32_t g_font_regular, g_font_bold;

static void rebuild(bool dark) {
    g_current = dark ? dark_theme() : light_theme();
    apply_metrics(&g_current);
    g_current.font_regular = g_font_regular;
    g_current.font_bold    = g_font_bold;
    g_init = true;
}

const struct ui_theme *ui_theme(void) {
    if (!g_init) rebuild(false);
    return &g_current;
}
void ui_theme_use_dark(bool dark) { g_dark_pref = dark; rebuild(dark); }

/* Interface size. Clamped hard: below ~0.8 the UI stops being legible and
 * above ~1.3 the chrome starts colliding with itself, and a preference that
 * can break the desktop is not a preference, it is a trap. */
void ui_theme_set_scale(float s) {
    if (s < 0.80f) s = 0.80f;
    if (s > 1.30f) s = 1.30f;
    if (s == g_ui_scale) return;
    g_ui_scale = s;
    struct color a = g_current.accent;     /* rebuild() resets the accent... */
    rebuild(g_dark_pref);
    ui_theme_set_accent(a);                /* ...so put the user's back */
}

/* Re-tint the accent without touching anything else. The accent is the one
 * place the design spends boldness, so it is also the one thing worth letting
 * a user choose -- and choosing it must not mean editing a palette. The three
 * derived tones move with it (hover brighter, soft as a low-alpha wash, and
 * the ink that goes ON the accent), because a user picking "Teal" is not
 * volunteering to pick four colours that agree. */
void ui_theme_set_accent(struct color c) {
    g_current.accent = c;
    g_current.accent_hover = (struct color){ c.r + (1.f - c.r) * 0.18f,
                                             c.g + (1.f - c.g) * 0.18f,
                                             c.b + (1.f - c.b) * 0.18f, c.a };
    g_current.accent_soft  = (struct color){ c.r, c.g, c.b, 0.18f };
    /* white on a dark accent, near-black on a light one -- luminance decides,
     * not taste */
    float l = c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f;
    g_current.on_accent = l > 0.62f ? (struct color){ 0.08f, 0.08f, 0.09f, 1.f }
                                    : (struct color){ 1.f, 1.f, 1.f, 1.f };
}
void ui_theme_set_fonts(uint32_t regular, uint32_t bold) {
    g_font_regular = regular; g_font_bold = bold;
    if (!g_init) rebuild(false);
    g_current.font_regular = regular; g_current.font_bold = bold;
}
