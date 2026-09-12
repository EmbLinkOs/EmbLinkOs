/* ui/kit/kit.c -- the themed widget kit (see kit.h). Everything here composes
 * ui/declare primitives and pulls every dimension/colour from ui/theme. */

#include "kit.h"
#include "font.h"      /* glyph advances: a caret has to know where it is */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TH (ui_theme())

/* One colour from the control palette, or the theme's when unset. Declared up
 * here because ui_field is above the palette's storage. */
static struct ui_ctl_palette g_ctl;
static int g_ctl_on;
#define CP(field, dflt) ((g_ctl_on && g_ctl.field.a > 0) ? g_ctl.field : (dflt))

static struct layout_size sz_fixed(float v) { return (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = v }; }
static struct layout_size sz_grow(void)     { return (struct layout_size){ .mode = SIZE_FLEX, .flex_grow = 1 }; }
static struct layout_size sz_flex(float w)  { return (struct layout_size){ .mode = SIZE_FLEX, .flex_grow = w }; }
static struct layout_size sz_intrinsic(void){ return (struct layout_size){ .mode = SIZE_INTRINSIC }; }

static struct paint solid(struct color c) {
    struct paint p; p.kind = PAINT_SOLID; p.solid = c; p.n_stops = 0; return p;
}

/* --- structure ---------------------------------------------------------- */

void ui_screen_begin(void) {
    ui_begin_vstack(0);
    const struct ui_theme *t = TH;
    ui_set_paint(solid(t->bg));
    ui_set_size(sz_grow(), sz_grow());
    ui_set_padding(t->sp6, t->sp6, t->sp6, t->sp6);
    ui_set_spacing(t->sp5);
}
void ui_screen_end(void) { ui_end_stack(); }

void ui_card_begin(uint64_t key) {
    ui_box_begin(key);
    const struct ui_theme *t = TH;
    ui_set_paint(solid(t->surface));
    ui_set_corner_radius(t->radius_lg);
    ui_set_border(1.0f, t->border);
    ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
    ui_set_padding(t->sp5, t->sp5, t->sp5, t->sp5);
    ui_set_spacing(t->sp4);
    ui_set_size(sz_intrinsic(), sz_intrinsic());
    ui_set_axis(AXIS_COLUMN);   /* card is a column */
    ui_set_align(ALIGN_STRETCH);
}
void ui_card_end(void) { ui_box_end(); }

void ui_panel_begin(uint64_t key) {
    ui_box_begin(key);
    const struct ui_theme *t = TH;
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp4, t->sp4, t->sp4, t->sp4);
    ui_set_spacing(t->sp3);
    ui_set_axis(AXIS_COLUMN);
    ui_set_align(ALIGN_STRETCH);
}
void ui_panel_end(void) { ui_box_end(); }

void ui_row_begin(uint64_t key) {
    ui_begin_hstack(key);
    ui_set_spacing(TH->sp3);
    ui_set_align(ALIGN_CENTER);
}
void ui_row_end(void) { ui_end_stack(); }
void ui_col_begin(uint64_t key) { ui_begin_vstack(key); ui_set_spacing(TH->sp2); }
void ui_col_end(void) { ui_end_stack(); }

void ui_divider(void) {
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(t->border));
    ui_set_size(sz_grow(), sz_fixed(1));
    ui_box_end();
}
void ui_flex_spacer(void) { ui_spacer(); }
void ui_gap(float px) {
    ui_box_begin(0);
    ui_set_size(sz_fixed(px), sz_fixed(px));
    ui_box_end();
}

/* --- typography --------------------------------------------------------- */

static void text_role(uint32_t font, float size, struct color color, const char *s) {
    ui_set_font(font);
    ui_set_text_size(size);
    ui_set_text_color(color);
    ui_text("%s", s);
}
#define FMT(buf) do { va_list ap; va_start(ap, fmt); vsnprintf((buf), sizeof(buf), fmt, ap); va_end(ap); } while (0)

void ui_heading(const char *fmt, ...) { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_bold, t->text_heading, t->text, b); }
void ui_title(const char *fmt, ...)   { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_bold, t->text_title, t->text, b); }
void ui_body(const char *fmt, ...)    { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_regular, t->text_body, t->text, b); }
void ui_secondary(const char *fmt, ...){ char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_regular, t->text_body, t->text_secondary, b); }
void ui_caption(const char *fmt, ...) { char b[256]; FMT(b); const struct ui_theme *t=TH; text_role(t->font_regular, t->text_caption, t->text_tertiary, b); }

/* --- buttons ------------------------------------------------------------ */

/* Scale a colour's brightness (k>1 lighter, k<1 darker) for hover/press feedback. */
static struct color shade(struct color c, float k) {
    struct color o = { c.r * k, c.g * k, c.b * k, c.a };
    if (o.r > 1) o.r = 1;
    if (o.g > 1) o.g = 1;
    if (o.b > 1) o.b = 1;
    return o;
}

static bool button_impl(const char *label, struct color fill, struct color text_c,
                        float border_w, struct color border_c, bool has_fill) {
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    struct instance_handle self = ui_open();
    bool pressed = ui_is_pressed(), hovered = ui_is_hovered();
    if (has_fill) {
        struct color f = pressed ? shade(fill, 0.86f) : hovered ? shade(fill, 1.10f) : fill;
        ui_set_paint(solid(f));
    } else if (hovered) {                 /* ghost: soft wash on hover */
        ui_set_paint(solid(shade(t->accent_soft, pressed ? 0.9f : 1.0f)));
    } else {
        ui_set_paint(solid((struct color){0, 0, 0, 0}));   /* reset: un-hover cleanly */
    }
    ui_set_corner_radius(t->radius_md);
    if (border_w > 0) ui_set_border(border_w, hovered ? t->accent : border_c);
    ui_set_padding(t->sp2 + 1, t->sp4, t->sp2 + 1, t->sp4);
    ui_set_align(ALIGN_CENTER);
    text_role(t->font_bold, t->text_body, text_c, label);
    ui_box_end();
    return ui_consume_click(self);
}
bool ui_button_primary(const char *label) {
    const struct ui_theme *t = TH;
    return button_impl(label, t->accent, t->on_accent, 0, t->accent, true);
}
bool ui_button_secondary(const char *label) {
    const struct ui_theme *t = TH;
    return button_impl(label, t->surface, t->text, 1.0f, t->border_strong, true);
}
bool ui_button_ghost(const char *label) {
    const struct ui_theme *t = TH;
    return button_impl(label, t->accent, t->accent, 0, t->accent, false);
}

/* --- toggle ------------------------------------------------------------- */

bool ui_toggle(bool on) {
    const struct ui_theme *t = TH;
    /* track: a pill, accent when on / surface_alt when off, knob pushed to the
     * far end via justify (END when on, START when off) -- pure flex, no
     * absolute positioning. */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    struct color track = on ? t->accent : t->border_strong;
    if (ui_is_hovered()) track = shade(track, 1.12f);   /* brighten on hover */
    ui_set_paint(solid(track));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(42), sz_fixed(24));
    ui_set_padding(3, 3, 3, 3);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(on ? JUSTIFY_END : JUSTIFY_START);
    /* knob */
    ui_box_begin(0);
    ui_set_paint(solid(t->on_accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(18), sz_fixed(18));
    ui_box_end();
    ui_end_stack();
    return ui_consume_click(self);
}

/* --- checkbox / radio --------------------------------------------------- */

bool ui_checkbox(bool on) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), press = ui_is_pressed();
    struct color acc  = CP(focus, t->accent);
    struct color surf = CP(surface, t->surface_alt);
    struct color fill = on ? (press ? shade(acc, 0.86f) : acc)
                           : (hov  ? shade(surf, 1.15f) : surf);
    ui_set_paint(solid(fill));
    ui_set_corner_radius(t->radius_sm);
    ui_set_size(sz_fixed(20), sz_fixed(20));
    if (!on) ui_set_border(1.0f, hov ? acc : CP(border, t->border_strong));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    if (on) text_role(t->font_bold, t->text_caption, t->on_accent, "\xE2\x9C\x93");  /* U+2713 check */
    ui_end_stack();
    return ui_consume_click(self);
}

bool ui_radio(bool selected) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered();
    ui_set_paint(solid(selected ? t->accent_soft : CP(surface, t->surface_alt)));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(20), sz_fixed(20));
    ui_set_border(1.5f, selected ? CP(focus, t->accent)
                                 : (hov ? CP(focus, t->accent) : CP(border, t->border_strong)));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    if (selected) {
        ui_box_begin(0);
        ui_set_paint(solid(CP(focus, t->accent)));
        ui_set_corner_radius(t->radius_pill);
        ui_set_size(sz_fixed(10), sz_fixed(10));
        ui_box_end();
    }
    ui_end_stack();
    return ui_consume_click(self);
}

/* --- progress / slider -------------------------------------------------- */

/* A pill track with an accent-filled leading portion. Fill width is `frac` of
 * the track via flex weights, so it works at any track width (no absolute pos). */
void ui_progress(float frac) {
    const struct ui_theme *t = TH;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_grow(), sz_fixed(8));
    ui_set_align(ALIGN_STRETCH);
    ui_box_begin(0);                          /* filled leading portion */
    ui_set_paint(solid(t->accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_flex(frac > 0.001f ? frac : 0.001f), sz_grow());
    ui_box_end();
    ui_box_begin(0);                          /* remainder (transparent) */
    ui_set_size(sz_flex(1.0f - frac + 0.001f), sz_fixed(1));
    ui_box_end();
    ui_end_stack();
}

/* Drag anywhere on the full-height hit strip; the pointer maps into the track's
 * world rect (captured, so the drag continues even off the strip). */
float ui_slider(float value) {
    const struct ui_theme *t = TH;
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    ui_begin_hstack(0);
    (void)ui_open();
    float out = value;
    if (ui_is_active()) {
        float rx, ry, rw, rh, px, py;
        if (ui_open_rect(&rx, &ry, &rw, &rh) && rw > 0) {
            ui_pointer_pos(&px, &py);
            out = (px - rx) / rw;
            if (out < 0) out = 0;
            if (out > 1) out = 1;
        }
    }
    ui_set_size(sz_grow(), sz_fixed(24));     /* tall hit strip */
    ui_set_align(ALIGN_CENTER);
    ui_box_begin(0);                          /* filled leading portion */
    ui_set_paint(solid(t->accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_flex(value > 0.001f ? value : 0.001f), sz_fixed(4));
    ui_box_end();
    ui_box_begin(0);                          /* knob */
    ui_set_paint(solid(t->on_accent));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, ui_is_hovered() || ui_is_active() ? t->accent : t->border_strong);
    ui_set_size(sz_fixed(18), sz_fixed(18));
    ui_box_end();
    ui_box_begin(0);                          /* remainder track */
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_flex(1.0f - value + 0.001f), sz_fixed(4));
    ui_box_end();
    ui_end_stack();
    return out;
}

/* --- segmented control (tabs) ------------------------------------------- */

int ui_segmented(const char *const *labels, int count, int selected) {
    const struct ui_theme *t = TH;
    int result = selected;
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_padding(3, 3, 3, 3);
    ui_set_spacing(2);
    for (int i = 0; i < count; i++) {
        ui_box_begin((uint64_t)(i + 1));
        struct instance_handle seg = ui_open();
        bool sel = (i == selected);
        if (sel) {
            ui_set_paint(solid(t->surface));
            ui_set_shadow(true, t->shadow_sm.dx, t->shadow_sm.dy, t->shadow_sm.blur, t->shadow_sm.color);
        } else if (ui_is_hovered()) {
            ui_set_paint(solid(shade(t->surface_alt, 1.10f)));
        } else {
            ui_set_paint(solid((struct color){0, 0, 0, 0}));   /* reset: un-hover cleanly */
        }
        ui_set_corner_radius(t->radius_sm);
        ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
        ui_set_align(ALIGN_CENTER);
        ui_set_justify(JUSTIFY_CENTER);
        text_role(t->font_bold, t->text_body, sel ? t->text : t->text_secondary, labels[i]);
        ui_box_end();
        if (ui_consume_click(seg)) result = i;
    }
    ui_end_stack();
    return result;
}

/* --- chip / avatar ------------------------------------------------------ */

bool ui_chip(const char *label, bool active) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered();
    struct color bg = active ? t->accent_soft : (hov ? shade(t->surface_alt, 1.12f) : t->surface_alt);
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, active ? t->accent : t->border);
    ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
    ui_set_align(ALIGN_CENTER);
    text_role(t->font_bold, t->text_caption, active ? t->accent : t->text_secondary, label);
    ui_end_stack();
    return ui_consume_click(self);
}

void ui_avatar(const char *initials) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_paint(solid(t->accent_soft));
    ui_set_corner_radius(t->radius_pill);
    ui_set_size(sz_fixed(36), sz_fixed(36));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    text_role(t->font_bold, t->text_body, t->accent, initials);
    ui_end_stack();
}

/* --- text field --------------------------------------------------------- */

/* Enter, seen by the field that had focus when it was typed.
 *
 * One global rather than per-field state, because exactly one field can hold
 * focus -- so exactly one field can see a Return. The caller reads it right
 * after emitting its field, and reading CLEARS it: a submit is an edge, and an
 * edge that stays set fires again on every later frame. */
static bool g_field_submit;

bool ui_text_field_submitted(void) { bool s = g_field_submit; g_field_submit = false; return s; }

/* An emphasised span inside the next field's value, drawn when it is NOT being
 * edited: the span in the normal text colour, everything around it dimmed.
 *
 * It exists for one job -- an address bar has to make the HOST legible and let
 * the scheme and path recede, because the host is the part that says who you
 * are actually talking to, and the part a hostile URL pads out to hide. A field
 * that renders its value in one flat colour cannot say that.
 *
 * One-shot, like the submit edge: set immediately before the field, consumed by
 * it. While focused the value is drawn plain, because what you are editing is
 * the whole string and dimming two thirds of it would be lying about that. */
static unsigned g_emph_start, g_emph_len;
static bool     g_emph_on;

void ui_text_field_emphasis(unsigned start, unsigned len) {
    g_emph_start = start; g_emph_len = len; g_emph_on = (len > 0);
}

/* KEYBOARD FOCUS TRAVERSAL -- Tab moves to the next field.
 *
 * A form you can only fill by clicking each field is not usable from the
 * keyboard, and the login screen is exactly such a form. Immediate mode has no
 * list of focusable widgets to walk, so traversal is built from what it does
 * have: the order fields are emitted in, which IS the reading order.
 *
 *   Tab in the focused field  -> the NEXT field emitted this frame takes focus,
 *                                and whatever was typed after the Tab goes to it
 *   Tab in the LAST field     -> next frame, the FIRST field takes focus (wrap)
 *
 * Per-frame state, reset when ui_frame_serial() moves. */
static uint64_t g_trav_frame;
static bool     g_tab_pending;            /* hand focus to the next field emitted */
static bool     g_focus_first;            /* wrapped: the first field of this frame takes it */
static bool     g_seen_field;             /* a field has been emitted this frame */
static char     g_carry[32];              /* keys typed after the Tab, for the next field */
static int      g_carry_n;
static bool     g_autofocus_next;         /* see ui_text_field_autofocus */

void ui_text_field_autofocus(void) { g_autofocus_next = true; }

/* --- THE CARET -----------------------------------------------------------
 *
 * Until this existed a text field in this system could only be APPENDED to.
 * Typing went on the end, backspace came off the end, and the thing drawn as a
 * caret was a 2px box emitted after the whole string -- so it was not a cursor,
 * it was a decoration that happened to sit where the next character would go.
 * A typo three characters back meant deleting everything after it. Every
 * filename in the Open/Save panel, every search box, every setting in this OS
 * was edited that way.
 *
 * ONE CARET, NOT ONE PER FIELD, because only the focused field can have one and
 * exactly one field is focused. It is anchored to the BUFFER, not to the
 * widget's instance handle: a field is identified by the text it edits, which
 * is stable across frames by construction and survives the field moving in the
 * tree -- where an instance handle need not. Focus a different field and the
 * caret adopts it, at the end of its text, which is where someone clicking into
 * a field to add to it expects to be.
 *
 * BYTES, NOT CHARACTERS. The stream is UTF-8 (kernel/drivers/input/keyboard.c),
 * so the offset is a byte offset that is always kept on a character boundary --
 * a caret between the two bytes of an é is a caret that can split it in half. */
static char    *g_caret_buf;              /* whose caret g_caret is */
static unsigned g_caret;                  /* byte offset into it */

/* THE SELECTION is an ANCHOR plus the caret, not a start and an end.
 *
 * The anchor is where the selection was begun and the caret is where it has
 * been dragged to, so the caret can be on either side of it -- which is the
 * whole behaviour of shift-arrowing left past your starting point and back
 * again. Storing a start and an end instead loses which end is moving, and a
 * selection that cannot shrink from the side you grew it is one people
 * immediately notice. `anchor == caret` means there is no selection. */
static unsigned g_anchor;

static unsigned sel_lo(void) { return g_anchor < g_caret ? g_anchor : g_caret; }
static unsigned sel_hi(void) { return g_anchor < g_caret ? g_caret : g_anchor; }
static bool     has_sel(void) { return g_anchor != g_caret; }

static int (*g_clip_set)(const char *, unsigned);
static int (*g_clip_get)(char *, unsigned);

void ui_clipboard_provider(int (*set)(const char *, unsigned),
                           int (*get)(char *, unsigned)) {
    g_clip_set = set; g_clip_get = get;
}

/* Continuation bytes are 10xxxxxx: never a character's first byte. */
static bool cp_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static unsigned utf8_back(const char *s, unsigned i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && cp_cont(s[i])) i--;
    return i;
}
static unsigned utf8_fwd(const char *s, unsigned len, unsigned i) {
    if (i >= len) return len;
    i++;
    while (i < len && cp_cont(s[i])) i++;
    return i;
}

/* Width of the first `nbytes` of `s`. Steps the string with the RENDERER's own
 * decoder -- anything that measures text differently from the way it is drawn
 * puts the caret somewhere the glyphs are not. */
float ui_text_width_n(uint32_t font_handle, float size_px, const char *s, int nbytes) {
    if (!s || !*s || nbytes == 0) return 0.0f;
    struct font *f = font_for_handle(font_handle);
    if (!f) return 0.0f;
    unsigned limit = nbytes < 0 ? (unsigned)-1 : (unsigned)nbytes;
    float w = 0;
    for (unsigned i = 0; s[i] && i < limit; ) {
        uint32_t cp;
        int adv = font_utf8_decode(s + i, &cp);
        if (adv <= 0) break;
        struct glyph_cache_entry *e =
            glyph_cache_lookup_or_rasterize(font_global_atlas(), f, cp, size_px);
        if (e) w += e->advance_px;
        i += (unsigned)adv;
    }
    return w;
}

/* The character boundary nearest `x`, measured from the text's left edge.
 * NEAREST, not "the one it landed inside": clicking the right half of a letter
 * must put the caret AFTER it, which is what everyone means by pointing between
 * two characters. */
static unsigned caret_from_x(const char *s, unsigned len, uint32_t font,
                             float size_px, float dx) {
    if (dx <= 0) return 0;
    float prev = 0;
    for (unsigned i = 0; i < len; ) {
        unsigned nx = utf8_fwd(s, len, i);
        float w = ui_text_width_n(font, size_px, s, (int)nx);
        if (dx < (prev + w) * 0.5f) return i;
        prev = w;
        i = nx;
    }
    return len;
}

static void trav_new_frame(void) {
    uint64_t f = ui_frame_serial();
    if (f == g_trav_frame) return;
    g_trav_frame = f;
    if (g_tab_pending) {                  /* Tab in the last field last frame: wrap */
        g_tab_pending = false;
        g_focus_first = true;
    }
    g_seen_field = false;
}

static bool ui_field(char *buf, unsigned long cap, const char *placeholder, bool masked) {
    const struct ui_theme *t = TH;
    bool     emph  = g_emph_on;
    unsigned emph_s = g_emph_start, emph_n = g_emph_len;
    g_emph_on = false;                    /* consumed, whether or not it is used */
    bool autofocus = g_autofocus_next;
    g_autofocus_next = false;             /* one-shot, like the emphasis */
    trav_new_frame();
    bool first = !g_seen_field;
    g_seen_field = true;

    ui_box_begin(0);
    struct instance_handle self = ui_open();
    bool clicked = ui_consume_click(self);
    if (clicked) ui_request_focus(self);
    /* How this field came by focus decides where its keys come from. The typed
     * queue is the FRAME's, not consumed per field: a field Tabbed into in the
     * same frame must take only what followed the Tab (the carry) -- taking the
     * queue again would replay everything, the Tab included, and bounce focus
     * down the whole form. A field wrapped into on the NEXT frame takes the
     * carry and then that frame's queue, which is new. */
    enum { OWN_QUEUE, CARRY_ONLY, CARRY_THEN_QUEUE } keys = OWN_QUEUE;
    if (g_tab_pending) {                  /* the previous field was Tabbed out of */
        g_tab_pending = false;
        ui_request_focus(self);
        keys = CARRY_ONLY;
    } else if (first && g_focus_first) {  /* wrapped around from the last field */
        g_focus_first = false;
        ui_request_focus(self);
        keys = CARRY_THEN_QUEUE;
    } else if (autofocus && !ui_any_focus()) {
        ui_request_focus(self);           /* a form that opens ready to type */
    }
    bool focused = ui_has_focus(self);

    /* THE CARET FOLLOWS FOCUS, and a click puts it where the pointer is.
     *
     * The well's rect is last frame's geometry, which is exactly right: the
     * click being handled happened over what was on screen then. The text
     * starts one left padding in -- the same t->sp3 set on this box a few lines
     * below, read here rather than guessed.
     *
     * A masked field gets the caret at the end wherever you click it: the
     * glyphs are asterisks, so an offset measured against them would be an
     * offset into a string that is not the one being edited. */
    unsigned len = (unsigned)strlen(buf);
    if (focused) {
        if (g_caret_buf != buf) { g_caret_buf = buf; g_caret = len; g_anchor = len; }
        if (g_caret > len) g_caret = len;
        if (g_anchor > len) g_anchor = len;
        /* The app may have rewritten the buffer under us (the file panel fills
         * the name field when you click a file). Never leave the caret inside a
         * character. */
        while (g_caret > 0 && g_caret < len && cp_cont(buf[g_caret])) g_caret--;
        while (g_anchor > 0 && g_anchor < len && cp_cont(buf[g_anchor])) g_anchor--;

        if (clicked) {
            float rx, ry, rw, rh, px, py;
            if (!masked && ui_open_rect(&rx, &ry, &rw, &rh) && rw > 0) {
                ui_pointer_pos(&px, &py);
                (void)ry; (void)rh; (void)py;
                g_caret = caret_from_x(buf, len, t->font_regular, t->text_body,
                                       px - (rx + t->sp3));
            } else {
                g_caret = len;
            }
            g_anchor = g_caret;        /* a click starts a fresh, empty selection */
        }
    }

    /* while focused, apply this frame's typed characters in place -- first
     * whatever the field before this one saw typed after a Tab */
    if (focused) {
        char in[64];
        int n = 0;
        if (keys != OWN_QUEUE) {
            memcpy(in, g_carry, (size_t)g_carry_n);
            n = g_carry_n;
            g_carry_n = 0;
        }
        if (keys != CARRY_ONLY)
            n += ui_input_take(in + n, (int)sizeof in - n);
        /* Cut the selection out, leaving the caret where it was. Every edit
         * that writes goes through this first: typing over a selection, and
         * backspace or delete with one, all mean "this text goes away". */
        #define DROP_SEL() do { \
            if (has_sel()) { \
                unsigned lo = sel_lo(), hi = sel_hi(); \
                memmove(buf + lo, buf + hi, len - hi + 1); \
                len -= hi - lo; \
                g_caret = g_anchor = lo; \
            } \
        } while (0)

        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)in[i];
            if (c == UI_KEY_SEL_LEFT)  { g_caret = utf8_back(buf, g_caret); }
            else if (c == UI_KEY_SEL_RIGHT) { g_caret = utf8_fwd(buf, len, g_caret); }
            else if (c == UI_KEY_SEL_HOME)  { g_caret = 0; }
            else if (c == UI_KEY_SEL_END)   { g_caret = len; }
            else if (c == UI_KEY_SEL_ALL)   { g_anchor = 0; g_caret = len; }
            else if (c == UI_KEY_COPY || c == UI_KEY_CUT) {
                /* Nothing selected is not an error and must not clear what is
                 * already on the clipboard: a mis-aimed copy that wiped it
                 * would lose the thing you were about to paste. */
                if (has_sel() && g_clip_set && !masked)
                    g_clip_set(buf + sel_lo(), sel_hi() - sel_lo());
                /* A masked field never yields its text -- the point of masking
                 * it is that it does not appear, and a clipboard is a place it
                 * would appear. The caret and selection still work; only this
                 * one operation refuses. */
                if (c == UI_KEY_CUT && !masked) DROP_SEL();
            }
            else if (c == '\b') {
                /* BACK OVER A WHOLE CHARACTER, not a byte. The stream is UTF-8
                 * (kernel/drivers/input/keyboard.c), so é is two bytes and
                 * deleting one of them leaves a half character the renderer
                 * draws as a replacement box -- one backspace, one visible
                 * thing removed, is the only behaviour anyone would call
                 * correct. */
                if (has_sel()) DROP_SEL();
                else if (g_caret > 0) {
                    unsigned p = utf8_back(buf, g_caret);
                    memmove(buf + p, buf + g_caret, len - g_caret + 1);
                    len -= g_caret - p;
                    g_caret = p;
                }
                g_anchor = g_caret;
            }
            else if (c == UI_KEY_DEL) {
                /* Delete is backspace's mirror: it takes the character AFTER
                 * the caret. With an append-only field there was nothing after
                 * the caret, so this key did nothing at all. */
                if (has_sel()) DROP_SEL();
                else if (g_caret < len) {
                    unsigned nx = utf8_fwd(buf, len, g_caret);
                    memmove(buf + g_caret, buf + nx, len - nx + 1);
                    len -= nx - g_caret;
                }
                g_anchor = g_caret;
            }
            /* A plain arrow COLLAPSES a selection to the edge it moves
             * toward, rather than moving the caret from wherever it happens to
             * be. Pressing Left with three words selected means "put me at the
             * start of them", which is what every editor does and what anyone
             * who has just selected something and changed their mind expects. */
            else if (c == UI_KEY_LEFT) {
                g_caret = has_sel() ? sel_lo() : utf8_back(buf, g_caret);
                g_anchor = g_caret;
            }
            else if (c == UI_KEY_RIGHT) {
                g_caret = has_sel() ? sel_hi() : utf8_fwd(buf, len, g_caret);
                g_anchor = g_caret;
            }
            else if (c == UI_KEY_HOME)  { g_caret = 0;   g_anchor = 0; }
            else if (c == UI_KEY_END)   { g_caret = len; g_anchor = len; }
            else if (c == UI_KEY_UP || c == UI_KEY_DOWN ||
                     c == UI_KEY_PGUP || c == UI_KEY_PGDN) {
                /* A one-line field has nowhere vertical to go. Swallowed rather
                 * than fallen through, so they cannot be mistaken for text. */
            }
            else if (c == '\n') { g_field_submit = true; }   /* submit; see above */
            else if (c == '\t') {
                /* The rest of this frame's keys belong to the next field. */
                g_carry_n = 0;
                for (int k = i + 1; k < n && g_carry_n < (int)sizeof g_carry; k++)
                    g_carry[g_carry_n++] = in[k];
                g_tab_pending = true;
                break;
            }
            else if (c == UI_KEY_PASTE) {
                /* The paste itself is the runtime's (ui/dsl/em_app.c): it holds
                 * the clipboard and replays it through this same queue, so that
                 * an app with a key hook -- a terminal, which must never let a
                 * pasted newline execute anything -- gets a say. What arrives
                 * here is the replayed text, one character at a time, and the
                 * first of them replaces the selection like any other typing.
                 * Swallowed rather than fallen through so the command byte
                 * itself is never mistaken for text. */
            }
            else if ((c >= 32 && c < 127) || c >= 0x80) {
                /* >= 0x80 is a UTF-8 byte of a real character -- é, €, a
                 * dead-key composition. Each byte is inserted as it arrives;
                 * they are contiguous in the queue and the caret advances one
                 * byte at a time, so a multi-byte character lands intact and in
                 * order. Rejecting them (the old `< 127` bound) is what made
                 * every accented character vanish between the keyboard and the
                 * field. */
                DROP_SEL();                 /* typing over a selection replaces it */
                if (len + 1 < cap) {
                    memmove(buf + g_caret + 1, buf + g_caret, len - g_caret + 1);
                    buf[g_caret++] = (char)c;
                    len++;
                    g_anchor = g_caret;
                }
            }
        }
        #undef DROP_SEL
    }

    /* the input well */
    ui_set_paint(solid(CP(surface, t->surface_alt)));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(focused ? 1.5f : 1.0f,
                  focused ? CP(focus, t->accent) : CP(border, t->border_strong));
    ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_align(ALIGN_CENTER);

    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    /* The runs of an emphasised value must sit flush; the 1px is the gap the
     * caret needs and there is no caret when this draws. */
    unsigned long emph_fits = 0;
    if (emph && !focused && !masked) {
        emph_fits = strlen(buf);
        if (emph_s >= emph_fits || emph_s + emph_n > emph_fits) emph_fits = 0;
    }
    ui_set_spacing(emph_fits ? 0 : 1);
    if (buf[0] == 0 && !focused) {
        text_role(t->font_regular, t->text_body, CP(placeholder, t->text_tertiary), placeholder);
    } else if (emph_fits) {
        /* dim head, bright span, dim tail -- up to three runs, any of which
         * may be empty and is then simply not emitted */
        char part[256];
        struct color dim = CP(placeholder, t->text_tertiary);
        struct color lit = CP(text, t->text);
        if (emph_s) {
            unsigned n = emph_s < sizeof part - 1 ? emph_s : (unsigned)sizeof part - 1;
            memcpy(part, buf, n); part[n] = 0;
            text_role(t->font_regular, t->text_body, dim, part);
        }
        {
            unsigned n = emph_n < sizeof part - 1 ? emph_n : (unsigned)sizeof part - 1;
            memcpy(part, buf + emph_s, n); part[n] = 0;
            text_role(t->font_regular, t->text_body, lit, part);
        }
        if (emph_s + emph_n < emph_fits) {
            const char *tail = buf + emph_s + emph_n;
            snprintf(part, sizeof part, "%s", tail);
            text_role(t->font_regular, t->text_body, dim, part);
        }
        ui_flex_spacer();
    } else {
        /* THE VALUE, SPLIT AROUND THE CARET.
         *
         * The caret used to be a 2px box emitted AFTER the whole string, which
         * is why it could only ever sit at the end -- the widget had no way to
         * put it anywhere else, so the field had no way to be edited anywhere
         * else. Two text runs with the box between them puts it at a real
         * offset and costs no measurement at all: the layout already knows how
         * wide the first run is, because it just laid it out.
         *
         * A masked field is masked by CHARACTER, not by byte: one asterisk per
         * character, so an accented password is as long on screen as it is in
         * the hand. */
        /* Up to three runs -- before the selection, the selection, after it --
         * with the caret box emitted at whichever boundary the caret is on.
         * The highlight is a property of the RUN (ui_set_text_bg) rather than a
         * rectangle drawn at a computed x, so it lands exactly where layout put
         * the words and needs no measurement to stay there. */
        struct color fg = CP(text, t->text);
        unsigned lo = focused ? sel_lo() : len;
        unsigned hi = focused ? sel_hi() : len;
        if (lo > len) lo = len;
        if (hi > len) hi = len;

        char run[256];
        /* A masked field is masked by CHARACTER, not by byte: one asterisk per
         * character, so an accented password is as long on screen as it is in
         * the hand. */
        #define EMIT(a, b, selected) do {                                       \
            unsigned n_ = 0;                                                    \
            for (unsigned i_ = (a); i_ < (b) && n_ < sizeof run - 1; i_++) {     \
                if (masked) { if (!cp_cont(buf[i_])) run[n_++] = '*'; }          \
                else        { run[n_++] = buf[i_]; }                            \
            }                                                                   \
            run[n_] = 0;                                                        \
            if (run[0]) {                                                       \
                if (selected) ui_set_text_bg(CP(selection, t->selection));      \
                text_role(t->font_regular, t->text_body, fg, run);              \
            }                                                                   \
        } while (0)

        #define CARET() do {                                                    \
            ui_box_begin(0);                                                    \
            ui_set_paint(solid(CP(focus, t->accent)));                          \
            ui_set_size(sz_fixed(2), sz_fixed(t->text_body));                   \
            ui_box_end();                                                       \
        } while (0)

        EMIT(0, lo, 0);
        if (focused && g_caret == lo) CARET();
        EMIT(lo, hi, 1);
        if (focused && g_caret == hi && hi != lo) CARET();
        EMIT(hi, len, 0);
        #undef EMIT
        #undef CARET
        ui_flex_spacer();                 /* keep text left-aligned in the well */
    }
    ui_end_stack();
    ui_box_end();
    return focused;
}

void ui_set_control_palette(const struct ui_ctl_palette *p) {
    if (p) { g_ctl = *p; g_ctl_on = 1; } else g_ctl_on = 0;
}

struct color ui_ctl_color_(int which, struct color dflt) {
    if (!g_ctl_on) return dflt;
    struct color c = which == UI_CTL_SURFACE     ? g_ctl.surface
                   : which == UI_CTL_BORDER      ? g_ctl.border
                   : which == UI_CTL_FOCUS       ? g_ctl.focus
                   : which == UI_CTL_TEXT        ? g_ctl.text
                   : which == UI_CTL_SELECTION   ? g_ctl.selection
                                                 : g_ctl.placeholder;
    return c.a > 0 ? c : dflt;
}

bool ui_text_field(char *buf, unsigned long cap, const char *placeholder) {
    return ui_field(buf, cap, placeholder, false);
}

bool ui_password_field(char *buf, unsigned long cap, const char *placeholder) {
    return ui_field(buf, cap, placeholder, true);
}

/* --- scroll view -------------------------------------------------------- */

static float *g_scroll_ptr;

void ui_scroll_begin(uint64_t key, float viewport_h, float *scroll_y) {
    const struct ui_theme *t = TH;
    ui_begin_vstack(key);
    /* wheel over the view scrolls it (40px per notch; wheel up -> content up) */
    float w = ui_take_wheel();
    if (w != 0.0f) *scroll_y -= w * 40.0f;
    if (*scroll_y < 0) *scroll_y = 0;
    ui_set_size(sz_grow(), sz_fixed(viewport_h));
    ui_set_clip_children(true);
    ui_set_scroll_offset(*scroll_y);
    ui_set_spacing(t->sp2);
    ui_set_align(ALIGN_STRETCH);
    g_scroll_ptr = scroll_y;
}
void ui_scroll_end(void) {
    /* clamp to content using last frame's measured extents (one-frame lag is ok) */
    float content = 0, viewport = 0;
    if (ui_open_content_extent(&content, &viewport)) {
        float maxs = content - viewport;
        if (maxs < 0) maxs = 0;
        if (g_scroll_ptr && *g_scroll_ptr > maxs) { *g_scroll_ptr = maxs; ui_set_scroll_offset(maxs); }
    }
    ui_end_stack();
}

/* --- overlay / modal ---------------------------------------------------- */

static struct instance_handle g_overlay_h, g_dialog_h;

void ui_overlay_begin(uint64_t key) {
    ui_begin_hstack(key);
    g_overlay_h = ui_open();
    struct color scrim = { 0.0f, 0.0f, 0.0f, 0.55f };   /* dim the content behind */
    ui_set_overlay(true);                 /* fill the screen, out of flow (paints on top) */
    ui_set_layer(1);                      /* elevated: paints above + hits above the flow */
    ui_set_paint(solid(scrim));
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
}
bool ui_overlay_end(void) {
    ui_end_stack();
    return ui_consume_click(g_overlay_h);   /* true only if the bare scrim was clicked */
}
void ui_dialog_begin(uint64_t key) {
    ui_card_begin(key);
    g_dialog_h = ui_open();
}
void ui_dialog_end(void) {
    ui_consume_click(g_dialog_h);           /* absorb clicks on the dialog so they don't dismiss */
    ui_card_end();
}

/* --- badge -------------------------------------------------------------- */

void ui_badge(const char *label, enum ui_badge_tone tone) {
    const struct ui_theme *t = TH;
    struct color fg, bg;
    switch (tone) {
        case BADGE_SUCCESS: fg = t->success; break;
        case BADGE_WARNING: fg = t->warning; break;
        case BADGE_DANGER:  fg = t->danger;  break;
        case BADGE_NEUTRAL: fg = t->text_secondary; break;
        case BADGE_ACCENT:
        default:            fg = t->accent; break;
    }
    bg = t->accent_soft;
    if (tone == BADGE_NEUTRAL) bg = t->surface_alt;
    ui_begin_hstack(0);
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_padding(t->sp1, t->sp2 + 2, t->sp1, t->sp2 + 2);
    ui_set_align(ALIGN_CENTER);
    text_role(t->font_bold, t->text_caption, fg, label);
    ui_end_stack();
}
