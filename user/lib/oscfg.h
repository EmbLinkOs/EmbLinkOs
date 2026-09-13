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

/* WHERE THE FILE LIVES, resolved once, from the environment.
 *
 * It used to be a fixed /data/settings.conf, and NO SESSION COULD REACH IT. A
 * session's namespace is `ro /system, ro /data/apps, rw /home/<user>, rw /run`
 * -- /data/settings.conf is in none of those -- so every oscfg_load() in the
 * desktop opened it, got -ENOENT, and silently used defaults. Measured, in the
 * shell, once per poll for the life of the machine:
 *
 *     home: cfg open=-2 wp=0 accent=0 dock=38
 *
 * The accent, the theme, the interface size, the dock size, the indicator
 * dots, the keyboard layout the shell re-applies at login: none of them had
 * ever been worn by the desktop. Settings wrote the file, and the only process
 * that could read it back was one started from the kernel console, which has
 * no namespace at all. That is why this looked like it worked.
 *
 * The user's own directory is where a per-user preference belongs, and
 * /home/<user> is ALREADY writable by that user's session -- so this is a fix
 * that needs no widening of the namespace policy, which is the kind of fix to
 * prefer when the alternative is punching a hole for one filename.
 *
 * The old path remains as the fallback for a process with no HOME: the kernel
 * console's `run`, and the image-build tools. */
#define OSCFG_FALLBACK "/data/settings.conf"
static inline const char *oscfg_path(void) {
    static char p[192];
    if (!p[0]) {
        const char *h = getenv("HOME");
        if (h && h[0]) snprintf(p, sizeof p, "%s/settings.conf", h);
        else            snprintf(p, sizeof p, "%s", OSCFG_FALLBACK);
    }
    return p;
}

/* The accents are named rather than free RGB: a palette a designer chose beats
 * a colour wheel a user fights, and every one of these is checked against the
 * dark ground the shell actually uses. */
#define OSCFG_ACCENTS 8
struct oscfg_accent { const char *name; float r, g, b; };
/* APPENDED TO, NEVER REORDERED. The file stores an INDEX, so moving an entry
 * silently repaints somebody's desktop a different colour the next time they
 * boot. New colours go on the end. */
static const struct oscfg_accent oscfg_accents[OSCFG_ACCENTS] = {
    { "Indigo",  0.42f, 0.45f, 0.94f },
    { "Teal",    0.16f, 0.68f, 0.63f },
    { "Amber",   0.92f, 0.66f, 0.20f },
    { "Rose",    0.90f, 0.36f, 0.48f },
    { "Violet",  0.65f, 0.42f, 0.92f },
    { "Blue",    0.20f, 0.55f, 0.96f },
    { "Green",   0.24f, 0.72f, 0.39f },
    { "Coral",   0.96f, 0.44f, 0.31f },
};

/* THE DESKTOP PICTURE. A path and a name, in one place, because the shell draws
 * it and Settings offers it and they must agree on the list -- a picker that
 * offers a file the desktop cannot find is a preference that silently does
 * nothing. Appended to, never reordered, for the same reason as the accents. */
#define OSCFG_WALLPAPERS 4
struct oscfg_wallpaper { const char *path; const char *label; };
static const struct oscfg_wallpaper oscfg_wallpapers[OSCFG_WALLPAPERS] = {
    { "/system/images/colibri-user.ppm",             "Graphite" },
    { "/system/images/hummingbird-wallpaper.ppm",    "Night"    },
    { "/system/images/hummingbird-wallpaper-v2.ppm", "Dusk"     },
    { "/system/images/login-wallpaper.ppm",          "Aurora"   },
};

/* THE KEYBOARD LAYOUTS THE KERNEL KNOWS, by the names its driver answers to.
 * An index rather than a string because this file's format is `key value` with
 * integer values, and because a user picks from a list -- there is no useful
 * layout you could type here that the driver would accept. */
#define OSCFG_KEYMAPS 3
struct oscfg_keymap { const char *name; const char *label; };
static const struct oscfg_keymap oscfg_keymaps[OSCFG_KEYMAPS] = {
    { "us",     "QWERTY (US)" },
    { "azerty", "AZERTY (French)" },
    { "dvorak", "Dvorak" },
};

struct oscfg {
    int accent;        /* index into oscfg_accents                       */
    int dark;          /* 1 dark, 0 light                                */
    int dock_size;     /* dock icon base size in px                      */
    int dock_dots;     /* light the socket behind a live app's icon      */
    int ui_scale;      /* interface size, PERCENT (80..130); 100 = default */
    int keymap;        /* index into oscfg_keymaps                       */
    int wallpaper;     /* index into oscfg_wallpapers                    */
    /* The menu bar. Four small knobs rather than one "style": each of these is
     * a separate question a person actually has an opinion about, and folding
     * them into presets would mean inventing combinations nobody asked for. */
    int bar_24h;       /* 1 = 24-hour clock, 0 = 12-hour with am/pm       */
    int bar_seconds;   /* show seconds in the clock                       */
    int bar_date;      /* show the date beside the time                   */
    int bar_cpu;       /* show the CPU readout                            */
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
    c->accent = 0; c->dark = 1; c->dock_size = 38; c->dock_dots = 1; c->keymap = 0;
    c->ui_scale = 100; c->wallpaper = 0;
    c->bar_24h = 1; c->bar_seconds = 0; c->bar_date = 1; c->bar_cpu = 1;
}

/* HOW THE SHELL'S NARROW COMPONENTS GET IT.
 *
 * The menu bar's namespace is `ro /system, rw /run`, and its manifest says so
 * on purpose: "read /system, reach /run -- nothing else nameable". It has no
 * business holding the user's home directory, and wanting to know whether the
 * clock shows seconds is not a reason to give it one.
 *
 * A published FILE under /run was the obvious answer and is not available:
 * /run is epfs, which holds endpoint nodes and nothing else, so creating a
 * regular file there fails -- measured, `home: publish rc=-1`, before this
 * comment replaced that design.
 *
 * So it goes over a CHANNEL, which is what /run is for. The desktop serves an
 * endpoint whose entire meaning is "connect and I will hand you the
 * preferences": no request payload, symmetric with the launcher endpoint next
 * to it where the connect is itself the signal. The reader gets the same text
 * the file holds, from the one serialiser, and needs no authority beyond the
 * /run it already has. */
#define OSCFG_ENDPOINT "/run/emlink.prefs"

/* Every value the file can carry, forced back into range. A hand-edited file
 * is a supported way to change these (Settings says so in About), so a typo
 * must yield a working desktop and not a 4000-pixel dock. */
static inline void oscfg_clamp(struct oscfg *c) {
    if (c->accent < 0 || c->accent >= OSCFG_ACCENTS) c->accent = 0;
    if (c->wallpaper < 0 || c->wallpaper >= OSCFG_WALLPAPERS) c->wallpaper = 0;
    if (c->dock_size < 28) c->dock_size = 28;
    if (c->dock_size > 60) c->dock_size = 60;
    if (c->ui_scale < 80)  c->ui_scale = 80;
    if (c->ui_scale > 130) c->ui_scale = 130;
    c->dark = !!c->dark; c->dock_dots = !!c->dock_dots;
    c->bar_24h = !!c->bar_24h; c->bar_seconds = !!c->bar_seconds;
    c->bar_date = !!c->bar_date; c->bar_cpu = !!c->bar_cpu;
}

static inline void oscfg_parse(struct oscfg *c, char *buf);
static inline int  oscfg_write_to(const struct oscfg *c, const char *path);

/* Ask the desktop for the preferences over OSCFG_ENDPOINT. Silent on failure:
 * a process that cannot reach the desktop keeps the defaults, exactly as one
 * that cannot open the file does. */
static inline void oscfg_ask_desktop(struct oscfg *c) {
    int ch = (int)embk_chan_connect(OSCFG_ENDPOINT);
    if (ch < 0) return;
    char buf[640];
    unsigned len = 0, nh = 0;
    if (embk_chan_recv(ch, buf, sizeof buf - 1, &len, 0, &nh) == 0 && len) {
        if (len > sizeof buf - 1) len = sizeof buf - 1;
        buf[len] = 0;
        oscfg_parse(c, buf);
    }
    embk_chan_close(ch);
}

/* Read one file into `c`, leaving it untouched if the file is unreadable.
 * Returns 1 if anything was parsed. */
static inline int oscfg_read_from(struct oscfg *c, const char *path) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    char b[1024];
    int64_t n = embk_read(fd, b, sizeof b - 1);
    embk_close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    oscfg_parse(c, b);
    return 1;
}

/* Read the preferences into `c`. The user's own file first, then the published
 * copy for a process whose namespace cannot name it. Missing/unreadable =>
 * defaults, never a failure: losing your preferences must not mean losing your
 * desktop. */
static inline void oscfg_load(struct oscfg *c) {
    oscfg_defaults(c);
    if (!oscfg_read_from(c, oscfg_path()))
        oscfg_ask_desktop(c);
    oscfg_clamp(c);
}

static inline void oscfg_parse(struct oscfg *c, char *buf) {
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
        else if (!strcmp(key, "keymap")) c->keymap = (val >= 0 && val < OSCFG_KEYMAPS) ? val : 0;
        else if (!strcmp(key, "ui_scale"))  c->ui_scale  = val;
        else if (!strcmp(key, "wallpaper")) c->wallpaper = val;
        else if (!strcmp(key, "bar_24h"))     c->bar_24h     = val;
        else if (!strcmp(key, "bar_seconds")) c->bar_seconds = val;
        else if (!strcmp(key, "bar_date"))    c->bar_date    = val;
        else if (!strcmp(key, "bar_cpu"))     c->bar_cpu     = val;
    }
}

/* Written whole rather than patched in place: the file is four lines, and a
 * rewrite cannot leave a half-updated one behind. */
/* The whole preference set as text. One serialiser, used by the file and by the
 * channel below: two encodings of the same thing is how they drift apart. */
static inline int oscfg_format(const struct oscfg *c, char *out, int cap) {
    return snprintf(out, (size_t)cap,
                     "# EmbLink preferences -- written by Settings, editable by hand.\n"
                     "accent %d\n" "dark %d\n" "dock_size %d\n" "dock_dots %d\n"
                     "ui_scale %d\n" "keymap %d\n" "wallpaper %d\n"
                     "bar_24h %d\n" "bar_seconds %d\n" "bar_date %d\n" "bar_cpu %d\n",
                     c->accent, c->dark, c->dock_size, c->dock_dots, c->ui_scale,
                     c->keymap, c->wallpaper,
                     c->bar_24h, c->bar_seconds, c->bar_date, c->bar_cpu);
}

static inline int oscfg_write_to(const struct oscfg *c, const char *path) {
    char out[640];
    int n = oscfg_format(c, out, (int)sizeof out);
    int fd = (int)embk_open(path, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (fd < 0) return -1;
    int64_t w = embk_write(fd, out, (size_t)n);
    embk_close(fd);
    return w == n ? 0 : -1;
}

static inline int oscfg_save(const struct oscfg *c) {
    return oscfg_write_to(c, oscfg_path());
}

/* Hand one connected peer the preferences and be done. Called by the desktop's
 * little server thread; see OSCFG_ENDPOINT above. */
static inline int oscfg_serve_one(int ch, const struct oscfg *c) {
    char out[640];
    int n = oscfg_format(c, out, (int)sizeof out);
    return embk_chan_send(ch, out, (unsigned)n, 0, 0, 0);
}

#endif /* _EMBLINK_OSCFG_H_ */
