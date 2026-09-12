#ifndef __EMBLINK_UI_KIT_H__
#define __EMBLINK_UI_KIT_H__

/* ui/kit/kit.h -- EmbLink UI: the themed widget kit.
 *
 * A thin, opinionated layer over the raw declarative API (ui/declare) that
 * reads exclusively from the design tokens (ui/theme). Its whole job is to make
 * the clean, coherent look the DEFAULT: an app author composes cards, labels,
 * buttons and toggles and gets consistent spacing, colour, radius and
 * elevation without touching a single raw value. */

#include "ui.h"
#include "theme.h"

/* --- structure --- */
void ui_screen_begin(void);   /* full-window page: bg fill, page padding, column */
void ui_screen_end(void);

void ui_card_begin(uint64_t key);   /* elevated surface: border + soft shadow + padding */
void ui_card_end(void);

void ui_panel_begin(uint64_t key);  /* quieter inset surface (surface_alt, hairline) */
void ui_panel_end(void);

void ui_row_begin(uint64_t key);    /* hstack, center-aligned, standard gap */
void ui_row_end(void);
void ui_col_begin(uint64_t key);    /* vstack, standard gap */
void ui_col_end(void);

void ui_divider(void);              /* full-width hairline rule */
void ui_flex_spacer(void);          /* pushes siblings apart on the main axis */
void ui_gap(float px);              /* fixed-size spacer */

/* --- typography (role == size + weight + colour from the tokens) --- */
void ui_heading(const char *fmt, ...);
void ui_title(const char *fmt, ...);
void ui_body(const char *fmt, ...);
void ui_secondary(const char *fmt, ...);
void ui_caption(const char *fmt, ...);

/* --- controls (return true on the frame they were clicked) --- */
bool ui_button_primary(const char *label);
bool ui_button_secondary(const char *label);
bool ui_button_ghost(const char *label);
bool ui_toggle(bool on);            /* returns true if clicked -> caller flips its state */

/* checkbox / radio: return true on the frame clicked -> caller flips its state */
bool ui_checkbox(bool on);
bool ui_radio(bool selected);

/* progress: a filled track, frac in [0,1], display-only */
void ui_progress(float frac);

/* slider: drag anywhere on the track; returns the (possibly updated) value [0,1] */
float ui_slider(float value);

/* segmented control (tabs): a row of `count` labels; returns the selected index,
 * updated to whatever was clicked this frame. */
int ui_segmented(const char *const *labels, int count, int selected);

/* chip: a compact, clickable pill (like a filter tag). true on the click frame. */
bool ui_chip(const char *label, bool active);

/* avatar: a tinted circle with 1-2 initials */
void ui_avatar(const char *initials);

/* text field: an editable single-line input. `buf` (cap bytes, NUL-terminated)
 * is the app-owned edit buffer; click to focus, then typed keys (fed via
 * ui_input_char in the loop) edit it in place. Draws a caret when focused and a
 * placeholder when empty+unfocused. Returns true while focused. */
/* A CONTROL PALETTE, for controls that are not part of the desktop.
 *
 * The kit's controls take their colours from the theme, which is right for an
 * application and wrong inside a rendered document: a web page is drawn on its
 * own white canvas, so a text field on it came out as a dark rounded box with
 * the desktop's accent -- the browser's chrome leaking into the page, which is
 * the same mistake as drawing the page's text in the desktop's ink.
 *
 * Set it around content that owns its own look and clear it afterwards, the
 * way a browser brackets the document it emits. NULL restores the theme. An
 * unset entry (alpha 0) falls back to the theme, so a caller can override one
 * colour without restating them all. */
struct ui_ctl_palette {
    struct color surface, border, focus, text, placeholder;
    /* Behind selected text. Left transparent (the usual case) it falls back to
     * the theme's soft accent, so a palette written before selections existed
     * keeps working and simply follows the theme here. */
    struct color selection;
};
void ui_set_control_palette(const struct ui_ctl_palette *p);

/* One colour from it, or `dflt` when the palette is unset or leaves that entry
 * transparent. For controls built OUTSIDE the kit (the DSL's Dropdown) that
 * still have to follow it. */
enum { UI_CTL_SURFACE, UI_CTL_BORDER, UI_CTL_FOCUS, UI_CTL_TEXT, UI_CTL_PLACEHOLDER,
       UI_CTL_SELECTION };
struct color ui_ctl_color_(int which, struct color dflt);

bool ui_text_field(char *buf, unsigned long cap, const char *placeholder);
bool ui_password_field(char *buf, unsigned long cap, const char *placeholder);

/* Did the field just emitted see a Return? Call it immediately after the field.
 * Reading clears it -- a submit is an EDGE, and an edge left set fires again on
 * every subsequent frame, which for a browser's address bar means reloading the
 * page forever. One field holds focus, so one flag is the whole state. */
bool ui_text_field_submitted(void);

/* Emphasise a span of the NEXT field's value while it is not being edited: that
 * span in the text colour, the rest dimmed. One-shot, consumed by the field.
 * An address bar uses it to make the host legible and let the scheme and path
 * recede -- the host being the part that says who you are talking to. */
void ui_text_field_emphasis(unsigned start, unsigned len);

/* The next field takes keyboard focus if nothing has it -- a form that opens
 * ready to type into (the login screen). One-shot, set before the field. */
void ui_text_field_autofocus(void);

/* The navigation keys, as BYTES in the same stream as typed text.
 *
 * A CONTRACT, not a convenience: these are the private codes the kernel's
 * keyboard driver emits for keys that have no character (kernel/drivers/input/
 * keyboard.h's EK_*, mirrored again for applications in user/lib/embk.h's
 * EMBK_KEY_*). All three must agree. They are repeated here rather than
 * included because this layer is built and unit-tested on the host, where
 * there is no kernel header to reach for -- which is also why kit_test can
 * drive a caret at all.
 *
 * They live in C0 because C0 is where a byte stream has room: the values are
 * chosen not to collide with Ctrl+letter, which owns 0x01-0x1A. */
#define UI_KEY_HOME  0x02
#define UI_KEY_END   0x05
#define UI_KEY_PGUP  0x0E
#define UI_KEY_PGDN  0x0F
#define UI_KEY_LEFT  0x11
#define UI_KEY_RIGHT 0x12
#define UI_KEY_UP    0x13
#define UI_KEY_DOWN  0x14
#define UI_KEY_DEL   0x7F

/* The editing commands, in the SAME stream as the text. 0xF8..0xFF can never
 * occur in valid UTF-8 at any position, which is what makes them safe to carry
 * here -- and carrying them here is what keeps them in ORDER with the typed
 * characters. A paste between two keystrokes has to land between them, and two
 * separate queues cannot promise that. */
#define UI_KEY_SEL_LEFT   0xF8
#define UI_KEY_SEL_RIGHT  0xF9
#define UI_KEY_SEL_HOME   0xFA
#define UI_KEY_SEL_END    0xFB
#define UI_KEY_SEL_ALL    0xFC
#define UI_KEY_COPY       0xFD
#define UI_KEY_CUT        0xFE
#define UI_KEY_PASTE      0xFF

/* THE CLIPBOARD, AS TWO FUNCTION POINTERS.
 *
 * The widget kit cannot call embk_clip_set: it is built and unit-tested on the
 * host, where there is no kernel to ask. So the application runtime installs
 * the real clipboard here (ui/dsl/em_app.c does it once at startup) and the
 * kit calls through. With nothing installed, copy and cut do nothing and the
 * field still works -- which is also how kit_test drives a clipboard of its
 * own and can test copying at all.
 *
 * `get` returns the number of bytes written, or <= 0 for an empty clipboard. */
void ui_clipboard_provider(int (*set)(const char *buf, unsigned len),
                           int (*get)(char *buf, unsigned cap));

/* Pixel width of the first `nbytes` of `s` (nbytes < 0 = the whole string),
 * stepped with the RENDERER's own UTF-8 decoder. Measuring text any other way
 * than it is drawn puts a caret where the glyphs are not. */
float ui_text_width_n(uint32_t font_handle, float size_px, const char *s, int nbytes);

/* --- scroll view --- */
/* A fixed-height viewport that clips + vertically scrolls its children. `scroll_y`
 * is the app-owned scroll position (px from top); the wheel over the view and a
 * drag inside it both update it, clamped to the content. Put content between. */
void ui_scroll_begin(uint64_t key, float viewport_h, float *scroll_y);
void ui_scroll_end(void);

/* --- overlay / modal --- */
/* A full-surface scrim that dims everything and centres its content. Declare it
 * LAST in the screen so it paints on top. ui_overlay_end() returns true when the
 * scrim (not the dialog) was clicked -- the app treats that as "dismiss". */
void ui_overlay_begin(uint64_t key);
bool ui_overlay_end(void);
void ui_dialog_begin(uint64_t key);   /* an elevated card centred in the overlay */
void ui_dialog_end(void);

/* --- accents --- */
enum ui_badge_tone { BADGE_ACCENT, BADGE_SUCCESS, BADGE_WARNING, BADGE_DANGER, BADGE_NEUTRAL };
void ui_badge(const char *label, enum ui_badge_tone tone);

#endif /* __EMBLINK_UI_KIT_H__ */
