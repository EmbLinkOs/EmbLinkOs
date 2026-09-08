/* user/lib/oscfg.h -- the system's user-visible preferences, and the one file
 * they live in.
 *
 * A settings app that only changes itself is a mock-up. For a preference to be
 * real, three parties have to agree on it: the app that WRITES it, the shell
 * that READS it live, and every application that should be wearing it. So the
 * schema lives here, in one header, rather than being re-parsed slightly
 * differently in three places.
 *
 * The format is deliberately the dullest thing that works -- "key value" lines
 * in /data/settings.conf. It is editable with the text editor we ship, it
 * diffs, and a corrupt or missing file simply yields defaults rather than an
 * unusable desktop. Unknown keys are preserved-by-ignoring: an older build
 * reading a newer file does not lose the keys it did not understand, it just
 * does not act on them.
 */
#ifndef _EMBLINK_OSCFG_H_
#define _EMBLINK_OSCFG_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "embk.h"

#define OSCFG_PATH "/data/settings.conf"

/* The accents are named rather than free RGB: a palette a designer chose beats
 * a colour wheel a user fights, and every one of these is checked against the
 * dark ground the shell actually uses. */
#define OSCFG_ACCENTS 5
struct oscfg_accent { const char *name; float r, g, b; };
static const struct oscfg_accent oscfg_accents[OSCFG_ACCENTS] = {
    { "Indigo",  0.42f, 0.45f, 0.94f },
    { "Teal",    0.16f, 0.68f, 0.63f },
    { "Amber",   0.92f, 0.66f, 0.20f },
    { "Rose",    0.90f, 0.36f, 0.48f },
    { "Violet",  0.65f, 0.42f, 0.92f },
};

struct oscfg {
    int accent;        /* index into oscfg_accents                       */
    int dark;          /* 1 dark, 0 light                                */
    int dock_size;     /* dock icon base size in px                      */
    int dock_dots;     /* show a running indicator under live apps       */
    int ui_scale;      /* interface size, PERCENT (80..130); 100 = default */
};

/* The screen band the DOCK owns, in pixels, for a given preference.
 *
 * This is a CONTRACT between the desktop that draws the dock and every app
 * that gets a window placed, and it exists because getting it wrong is not a
 * cosmetic bug. The dock is drawn by the desktop window, which sits BEHIND
 * every application window, and pointer input goes to the topmost window under
 * the cursor -- so an app window overlapping the dock silently swallows the
 * clicks aimed at it. The dock stops responding and nothing says why.
 *
 * It is a FUNCTION of the preference, not a constant, because the user can set
 * the dock icon size from 28 to 60. A hard-coded reserve is right at exactly
 * one setting and wrong at every other.
 *
 * pill (icons + padding) + the gap that keeps it floating rather than welded
 * to the screen edge.
 */
static inline int oscfg_dock_band(const struct oscfg *c) {
    int base = (c && c->dock_size > 0) ? c->dock_size : 38;
    if (base < 28) base = 28;
    if (base > 60) base = 60;
    return base + 32 + 14;
}

static inline void oscfg_defaults(struct oscfg *c) {
    c->accent = 0; c->dark = 1; c->dock_size = 38; c->dock_dots = 1;
    c->ui_scale = 100;
}

/* Read the file into `c`. Missing/unreadable => defaults, never a failure:
 * losing your preferences must not mean losing your desktop. */
static inline void oscfg_load(struct oscfg *c) {
    oscfg_defaults(c);
    int fd = (int)embk_open(OSCFG_PATH, EMBK_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[1024];
    int64_t n = embk_read(fd, buf, sizeof buf - 1);
    embk_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *p = buf; *p; ) {
        char key[32]; int val = 0;
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        if (line[0] == '#' || !line[0]) continue;
        if (sscanf(line, "%31s %d", key, &val) != 2) continue;
        if      (!strcmp(key, "accent"))    c->accent    = val;
        else if (!strcmp(key, "dark"))      c->dark      = val;
        else if (!strcmp(key, "dock_size")) c->dock_size = val;
        else if (!strcmp(key, "dock_dots")) c->dock_dots = val;
        else if (!strcmp(key, "ui_scale"))  c->ui_scale  = val;
    }
    if (c->accent < 0 || c->accent >= OSCFG_ACCENTS) c->accent = 0;
    if (c->dock_size < 28) c->dock_size = 28;
    if (c->dock_size > 60) c->dock_size = 60;
    if (c->ui_scale < 80)  c->ui_scale = 80;
    if (c->ui_scale > 130) c->ui_scale = 130;
}

/* Written whole rather than patched in place: the file is four lines, and a
 * rewrite cannot leave a half-updated one behind. */
static inline int oscfg_save(const struct oscfg *c) {
    char out[512];
    int n = snprintf(out, sizeof out,
                     "# EmbLink preferences -- written by Settings, editable by hand.\n"
                     "accent %d\n" "dark %d\n" "dock_size %d\n" "dock_dots %d\n"
                     "ui_scale %d\n",
                     c->accent, c->dark, c->dock_size, c->dock_dots, c->ui_scale);
    int fd = (int)embk_open(OSCFG_PATH, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (fd < 0) return -1;
    int64_t w = embk_write(fd, out, (size_t)n);
    embk_close(fd);
    return w == n ? 0 : -1;
}

#endif /* _EMBLINK_OSCFG_H_ */
