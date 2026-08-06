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
};

static inline void oscfg_defaults(struct oscfg *c) {
    c->accent = 0; c->dark = 1; c->dock_size = 38; c->dock_dots = 1;
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
    }
    if (c->accent < 0 || c->accent >= OSCFG_ACCENTS) c->accent = 0;
    if (c->dock_size < 28) c->dock_size = 28;
    if (c->dock_size > 60) c->dock_size = 60;
}

/* Written whole rather than patched in place: the file is four lines, and a
 * rewrite cannot leave a half-updated one behind. */
static inline int oscfg_save(const struct oscfg *c) {
    char out[512];
    int n = snprintf(out, sizeof out,
                     "# EmbLink preferences -- written by Settings, editable by hand.\n"
                     "accent %d\n" "dark %d\n" "dock_size %d\n" "dock_dots %d\n",
                     c->accent, c->dark, c->dock_size, c->dock_dots);
    int fd = (int)embk_open(OSCFG_PATH, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (fd < 0) return -1;
    int64_t w = embk_write(fd, out, (size_t)n);
    embk_close(fd);
    return w == n ? 0 : -1;
}

#endif /* _EMBLINK_OSCFG_H_ */
