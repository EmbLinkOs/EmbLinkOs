/* A Terminal profile: how a window looks and what it runs.
 *
 * A profile is a text file of `key = value` lines (docs/TERMINAL.md lists the
 * keys), kept in ~/.config/terminal/profiles; ~/.config/terminal/terminal.conf
 * names the default one. Terminal owns these files -- the Settings app's
 * schema rewrites drop keys it does not know -- and rewriting one keeps every
 * line it does not understand, comments included, where it was.
 *
 * The presets are compiled in, in the same format, and a file with a preset's
 * name takes its place. Pure: no file access here, so the host tests it. */
#ifndef TERM_PROFILE_H
#define TERM_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROFILE_NAME_MAX 64
#define PROFILE_TEXT_MAX 4096           /* a profile file's largest size */

enum profile_cursor { PROFILE_CURSOR_BLOCK, PROFILE_CURSOR_UNDERLINE, PROFILE_CURSOR_BAR };
enum profile_bell { PROFILE_BELL_VISUAL, PROFILE_BELL_NONE };

struct term_profile {
    char name[PROFILE_NAME_MAX];
    char font[64];                      /* a family: "MesloLGS Nerd Font Mono" or "DejaVu Sans Mono" */
    float font_size;                    /* pixels at the standard interface size */
    float line_spacing, letter_spacing; /* 1 = the font's own cell */
    bool bold_is_bright;                /* bold ANSI colours 0-7 draw as 8-15 */
    bool theme_colors;                  /* text, background and cursor follow the system theme */
    uint32_t color[16];                 /* 0xRRGGBB */
    uint32_t fg, bg, bold, selection, cursor;
    float opacity;                      /* of the background, 0.3-1 */
    float blur;                         /* of what is behind it, 0-1 */
    int padding;                        /* pixels around the screen */
    enum profile_cursor cursor_shape;
    bool cursor_blink;
    int columns, rows;                  /* a new window's size */
    int scrollback_mb;
    enum profile_bell bell;
    char command[128];                  /* the shell */
    char directory[192];                /* where a new tab starts: "same" (where the shown one is), "home", or a path */
    char term[32];                      /* TERM */
    bool clipboard_write;               /* programs may set the clipboard (OSC 52) */
};

/* The built-in profiles, in the file format. Clear Dark is first: the default. */
int profile_preset_count(void);
const char *profile_preset_text(int i);
const char *profile_preset_name(int i);
int profile_preset_find(const char *name);     /* -1 if none */

/* Clear Dark's values, then `text` over them. A key it does not know, or a
 * value out of range, is left as it was. Returns the keys it took. */
void profile_defaults(struct term_profile *p);
int profile_parse(struct term_profile *p, const char *text);
/* A preset by name over the defaults, false if there is none. */
bool profile_preset(struct term_profile *p, const char *name);

/* `p` written over `old` (NULL for a new file): each known key's line takes
 * its new value in place; other lines stay as they were; known keys missing
 * from `old` are added at the end. Returns the length, or -1 if `cap` is too
 * small. */
int profile_format(const struct term_profile *p, const char *old, char *out, size_t cap);

/* A file name for a profile name: lower case, a run of anything not a letter
 * or digit as one '-', ".conf" after it. */
void profile_file_name(const char *name, char *out, size_t cap);

/* One `key = value` from a small file such as terminal.conf; false if absent. */
bool profile_conf_get(const char *text, const char *key, char *out, size_t cap);
/* `text` with `key` set to `value` (replaced in place, or added). */
int profile_conf_set(const char *text, const char *key, const char *value, char *out, size_t cap);

/* DECSCUSR's code for the profile's cursor: 1-6. */
int profile_decscusr(const struct term_profile *p);

#endif
