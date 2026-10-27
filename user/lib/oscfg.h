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
 * in ~/settings.conf (with /data/settings.conf as the no-HOME fallback). It is editable with the text editor we ship, it
 * diffs, and a corrupt or missing file simply yields defaults rather than an
 * unusable desktop. Unknown keys are ignored while reading. Settings writes the complete
 * schema it understands; it does not preserve future or third-party keys.
 */
#ifndef _EMBLINK_OSCFG_H_
#define _EMBLINK_OSCFG_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "embk.h"
#include "display_arrange.h"

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

/* Curated accents retain stable indices. Custom RGB accents use a separate
 * key and are normalized by the shared theme for readable contrast. */
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
#define OSCFG_WALLPAPERS 8
#define OSCFG_WALLPAPER_PICTURES 4      /* the first N are files; the rest are drawn */
struct oscfg_wallpaper {
    const char *path;       /* NULL => a gradient the desktop DRAWS, see below */
    const char *label;
    float a[3], b[3], c[3]; /* three gradient stops, when path is NULL */
    float angle;            /* degrees */
};
/* PICTURES AND COLOURS, in one list, because to the person choosing there is
 * only one question. The colours are drawn rather than shipped: a 1600x1000
 * PPM is 4.8 MB and a gradient is nine floats, so the choice costs nothing on
 * an image that has to fit on a real machine. The renderer has had linear
 * gradients all along; nothing here needed inventing, only using. */
static const struct oscfg_wallpaper oscfg_wallpapers[OSCFG_WALLPAPERS] = {
    { "/system/images/colibri-user.ppm",             "Graphite" },
    { "/system/images/hummingbird-wallpaper.ppm",    "Night"    },
    { "/system/images/hummingbird-wallpaper-v2.ppm", "Dusk"     },
    { "/system/images/login-wallpaper.ppm",          "Aurora"   },
    { 0, "Indigo", { .09f,.10f,.22f }, { .20f,.18f,.46f }, { .05f,.05f,.12f }, 160.f },
    { 0, "Ember",  { .16f,.06f,.12f }, { .46f,.16f,.18f }, { .09f,.04f,.08f }, 150.f },
    { 0, "Pine",   { .04f,.14f,.13f }, { .09f,.30f,.26f }, { .03f,.07f,.08f }, 155.f },
    { 0, "Slate",  { .07f,.08f,.09f }, { .17f,.19f,.22f }, { .04f,.04f,.05f }, 165.f },
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

/* WHAT TIME IT IS HERE, as opposed to what time it is.
 *
 * The kernel's clock is UTC and stays UTC -- every timestamp on disk, every
 * inode, every log line. That is the right thing for a filesystem and the
 * wrong thing for a person, who wants the menu bar to agree with the clock on
 * their wall.
 *
 * A POSIX TZ STRING, NOT AN OLSON NAME. "Europe/Paris" is a lookup into a
 * database of every rule change since 1970 -- megabytes of it, revised several
 * times a year, needing a path in every namespace that wants a date. The TZ
 * string carries the rule itself: the offset, the abbreviation, and when
 * summer time starts and ends, in thirty characters that libc already knows
 * how to read. What it cannot do is tell you what the rule was in 1974, which
 * matters for a calendar application and does not matter for a clock.
 *
 * So the labels here name CITIES and the strings carry RULES, and the pair is
 * what a person picks from. Appended to, never reordered: the file stores an
 * index, and moving an entry moves somebody's clock. */
#define OSCFG_TIMEZONES 20
/* THE GROUP IS NOT DECORATION. Twenty entries do not fit in a dropdown that is
 * clipped by the window it opens in -- measured, the list ended at Athens and
 * the last eight zones could not be picked at all. So the picker asks two
 * questions, and this is the first one. */
#define OSCFG_TZ_GROUPS 3
static const char *const oscfg_tz_group_names[OSCFG_TZ_GROUPS] = {
    "Americas", "Europe & Africa", "Asia & Pacific"
};
struct oscfg_timezone { const char *label; const char *tz; int group; };
static const struct oscfg_timezone oscfg_timezones[OSCFG_TIMEZONES] = {
    /* Index 0 is UTC and is the default: a machine that has never been told
     * where it is should say so rather than guess a continent. */
    { "UTC",                     "UTC0", 1 },
    { "Honolulu",                "HST10", 0 },
    { "Los Angeles \xc2\xb7 Vancouver", "PST8PDT,M3.2.0,M11.1.0", 0 },
    { "Denver \xc2\xb7 Phoenix",        "MST7MDT,M3.2.0,M11.1.0", 0 },
    { "Chicago \xc2\xb7 Mexico City",   "CST6CDT,M3.2.0,M11.1.0", 0 },
    { "New York \xc2\xb7 Toronto",      "EST5EDT,M3.2.0,M11.1.0", 0 },
    { "S\xc3\xa3o Paulo",               "BRT3", 0 },
    { "London \xc2\xb7 Dublin \xc2\xb7 Lisbon", "GMT0BST,M3.5.0/1,M10.5.0/2", 1 },
    { "Lagos \xc2\xb7 Algiers",         "WAT-1", 1 },
    { "Paris \xc2\xb7 Berlin \xc2\xb7 Madrid", "CET-1CEST,M3.5.0,M10.5.0/3", 1 },
    { "Johannesburg \xc2\xb7 Cairo",    "SAST-2", 1 },
    { "Athens \xc2\xb7 Helsinki",       "EET-2EEST,M3.5.0/3,M10.5.0/4", 1 },
    { "Moscow \xc2\xb7 Istanbul",       "MSK-3", 1 },
    { "Dubai",                   "GST-4", 2 },
    { "Karachi",                 "PKT-5", 2 },
    { "Mumbai \xc2\xb7 Delhi",          "IST-5:30", 2 },
    { "Bangkok \xc2\xb7 Jakarta",       "WIB-7", 2 },
    { "Beijing \xc2\xb7 Singapore",     "CST-8", 2 },
    { "Tokyo \xc2\xb7 Seoul",           "JST-9", 2 },
    { "Sydney \xc2\xb7 Melbourne",      "AEST-10AEDT,M10.1.0,M4.1.0/3", 2 },
};


#define OSCFG_CONTOUR_FIELDS(X) \
    X(surface_style, 1, 0, 3) \
    X(custom_surface, 2107947, 0, 16777215) \
    X(accent_custom, 0, 0, 1) \
    X(custom_accent, 7452074, 0, 16777215) \
    X(contrast, 0, 0, 2) \
    X(density, 1, 0, 2) \
    X(font, 0, 0, 2) \
    X(text_scale, 100, 90, 130) \
    X(transparency, 35, 0, 50) \
    X(blur, 12, 0, 16) \
    X(reduce_motion, 0, 0, 1) \
    X(icon_theme, 0, 0, 1) \
    X(desktop_icons, 1, 0, 1) \
    X(desktop_icon_size, 48, 32, 64) \
    X(bar_autohide, 0, 0, 1) \
    X(shortcut_modifier, 0, 0, 1) \
    X(bar_edge, 0, 0, 1) \
    X(bar_size, 34, 28, 44) \
    X(bar_order, 0, 0, 1) \
    X(bar_network, 1, 0, 1) \
    X(bar_power, 1, 0, 1) \
    X(bar_sound, 1, 0, 1) \
    X(bar_workspaces, 1, 0, 1) \
    X(dock_edge, 0, 0, 2) \
    X(dock_autohide, 0, 0, 1) \
    X(dock_labels, 1, 0, 1) \
    X(cursor_style, 0, 0, 2) \
    X(cursor_size, 0, 0, 1) \
    X(decoration, 0, 0, 2) \
    X(window_radius, 8, 0, 12) \
    X(shadows, 1, 0, 2) \
    X(snap, 1, 0, 1) \
    X(workspace_count, 4, 1, 6) \
    X(workspace_wrap, 1, 0, 1)

struct oscfg {
    #define OSCFG_MEMBER(name, value, lo, hi) int name;
    OSCFG_CONTOUR_FIELDS(OSCFG_MEMBER)
    #undef OSCFG_MEMBER
    int volume,muted;
    int idle_dim_seconds,idle_lock_seconds,lid_lock,power_profile;
    int accent;        /* index into oscfg_accents                       */
    int dark;          /* 1 dark, 0 light                                */
    int dock_size;     /* dock icon base size in px                      */
    int dock_dots;     /* show the accent rail under a live app          */
    int ui_scale;      /* interface size, PERCENT (80..130); 100 = default */
    int keymap;        /* index into oscfg_keymaps                       */
    int wallpaper;     /* index into oscfg_wallpapers                    */
    int tz;            /* index into oscfg_timezones; 0 = UTC            */
    /* The menu bar. Four small knobs rather than one "style": each of these is
     * a separate question a person actually has an opinion about, and folding
     * them into presets would mean inventing combinations nobody asked for. */
    int bar_24h;       /* 1 = 24-hour clock, 0 = 12-hour with am/pm       */
    int bar_seconds;   /* show seconds in the clock                       */
    int bar_date;      /* show the date beside the time                   */
    int bar_cpu;       /* show the CPU readout                            */
    int display_width,display_height; /* confirmed per-user resolution; 0 = boot mode */
    /* The confirmed arrangement of several monitors, for the set connected
     * when it was kept (display_arrange.h); count 0 = none saved. */
    struct display_saved display_layout;
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

/* Work-area insets are shared by shell placement, app tiling and compositor.
 * Auto-hide retains its strip: a desktop-owned dock cannot cover app input. */
static inline void oscfg_insets(const struct oscfg *c, int *left, int *top, int *right, int *bottom) {
    int band=oscfg_dock_band(c);
    *left=c->dock_edge==1?band:0; *right=c->dock_edge==2?band:0;
    *top=c->bar_edge==0?c->bar_size:0;
    *bottom=c->bar_edge==1?c->bar_size:0;
    if(c->dock_edge==0)*bottom+=band;
}

static inline void oscfg_defaults(struct oscfg *c) {
    memset(c, 0, sizeof *c);
    #define OSCFG_DEFAULT(name, value, lo, hi) c->name = value;
    OSCFG_CONTOUR_FIELDS(OSCFG_DEFAULT)
    #undef OSCFG_DEFAULT
    c->volume=100;c->muted=0;
    c->idle_dim_seconds=120;c->idle_lock_seconds=300;c->lid_lock=1;c->power_profile=0;
    c->accent = 1; c->dark = 1; c->dock_size = 38; c->dock_dots = 1; c->keymap = 0;
    c->ui_scale = 100; c->wallpaper = 6; c->tz = 0;
    c->bar_24h = 1; c->bar_seconds = 0; c->bar_date = 1; c->bar_cpu = 0;
}

/* Curated presets only reset appearance; audio, accounts, time and input
 * choices survive. Defaults are also the reliable appearance reset. */
#define OSCFG_PRESETS 4
static const char *const oscfg_preset_names[] = { "Contour", "Paper", "Midnight", "Studio" };
static inline void oscfg_preset(struct oscfg *c, int preset) {
    int shortcut=c->shortcut_modifier;
    struct oscfg d; oscfg_defaults(&d);
    #define OSCFG_RESET(name, value, lo, hi) c->name=d.name;
    OSCFG_CONTOUR_FIELDS(OSCFG_RESET)
    #undef OSCFG_RESET
    c->shortcut_modifier=shortcut;
    c->accent=1;c->dark=1;c->ui_scale=100;c->wallpaper=6;c->dock_size=38;c->dock_dots=1;
    c->bar_cpu=0;c->bar_date=1;
    if(preset==1){c->dark=0;c->surface_style=1;c->accent=5;c->wallpaper=7;}
    if(preset==2){c->surface_style=2;c->accent=4;c->wallpaper=4;c->density=0;}
    if(preset==3){c->dark=0;c->surface_style=0;c->accent=0;c->wallpaper=7;c->density=0;c->transparency=0;c->reduce_motion=1;}
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
    if(c->display_width<640 || c->display_width>4096 || c->display_height<480 || c->display_height>2160 ||
       (uint64_t)c->display_width*c->display_height>8388608)c->display_width=c->display_height=0;
    {
        /* A saved arrangement is all or nothing: one bad entry and it is
         * dropped, and the monitors come up the way the kernel places them. */
        struct display_saved *l=&c->display_layout;int ok=l->count>=0 && l->count<=(int)EMBK_DISPLAY_MAX_OUTPUTS,primaries=0;
        l->mirror=!!l->mirror;
        for(int i=0;ok && i<l->count;i++) {
            if(!l->out[i].id || (l->out[i].enabled!=0 && l->out[i].enabled!=1) || (l->out[i].primary!=0 && l->out[i].primary!=1))ok=0;
            else if(l->out[i].enabled) {
                if(l->out[i].width<320 || l->out[i].width>8192 || l->out[i].height<200 || l->out[i].height>4320 ||
                   l->out[i].x<0 || l->out[i].x>8192 || l->out[i].y<0 || l->out[i].y>4320)ok=0;
                primaries+=l->out[i].primary;
            }else if(l->out[i].primary || l->out[i].width || l->out[i].height || l->out[i].x || l->out[i].y)ok=0;
        }
        if(!ok || (l->count && primaries!=1))memset(l,0,sizeof *l);
        for(int i=l->count;i<(int)EMBK_DISPLAY_MAX_OUTPUTS;i++)memset(&l->out[i],0,sizeof l->out[i]);
    }
    #define OSCFG_BOUND(name, value, lo, hi) if(c->name < lo)c->name=lo; if(c->name > hi)c->name=hi;
    OSCFG_CONTOUR_FIELDS(OSCFG_BOUND)
    #undef OSCFG_BOUND
    if(c->volume<0)c->volume=0;
    if(c->volume>100)c->volume=100;
    c->muted=!!c->muted;c->lid_lock=!!c->lid_lock;
    if(c->power_profile<0 || c->power_profile>2)c->power_profile=0;
    if(c->idle_dim_seconds<0 || c->idle_dim_seconds>86400 || (c->idle_dim_seconds && c->idle_dim_seconds<10))c->idle_dim_seconds=120;
    if(c->idle_lock_seconds<0 || c->idle_lock_seconds>86400 || (c->idle_lock_seconds && c->idle_lock_seconds<10))c->idle_lock_seconds=300;
    if (c->accent < 0 || c->accent >= OSCFG_ACCENTS) c->accent = 0;
    if (c->wallpaper < 0 || c->wallpaper >= OSCFG_WALLPAPERS) c->wallpaper = 0;
    if (c->tz < 0 || c->tz >= OSCFG_TIMEZONES) c->tz = 0;
    if (c->dock_size < 28) c->dock_size = 28;
    if (c->dock_size > 60) c->dock_size = 60;
    if (c->ui_scale < 80)  c->ui_scale = 80;
    if (c->ui_scale > 130) c->ui_scale = 130;
    c->dark = !!c->dark; c->dock_dots = !!c->dock_dots;
    c->bar_24h = !!c->bar_24h; c->bar_seconds = !!c->bar_seconds;
    c->bar_date = !!c->bar_date; c->bar_cpu = !!c->bar_cpu;
}

/* The rule for a preference, always valid -- an out-of-range index reads as
 * UTC rather than off the end of the table. */
static inline const char *oscfg_tz_string(const struct oscfg *c) {
    int i = (c && c->tz >= 0 && c->tz < OSCFG_TIMEZONES) ? c->tz : 0;
    return oscfg_timezones[i].tz;
}
static inline const char *oscfg_tz_label(const struct oscfg *c) {
    int i = (c && c->tz >= 0 && c->tz < OSCFG_TIMEZONES) ? c->tz : 0;
    return oscfg_timezones[i].label;
}

/* WEAR THE ZONE. TZ in the environment plus tzset() is the whole mechanism:
 * libc reads that variable and nothing else, so this is what makes localtime()
 * -- and therefore every clock, every file date, every `date` -- agree with
 * the preference.
 *
 * Guarded by a comparison because it is called from a poll loop once a second
 * for the life of the machine, and because setenv() frees and reallocates the
 * old string: doing that under a thread that is inside getenv() is a real
 * hazard, and not doing it at all unless the answer changed removes it for
 * every second but the one where the user actually picked a new zone. */
static inline void oscfg_apply_tz(const struct oscfg *c) {
    const char *want = oscfg_tz_string(c);
    const char *cur  = getenv("TZ");
    if (cur && !strcmp(cur, want)) return;
    setenv("TZ", want, 1);
    tzset();
}

static inline void oscfg_parse(struct oscfg *c, char *buf);
static inline int  oscfg_write_to(const struct oscfg *c, const char *path);

/* Ask the desktop for the preferences over OSCFG_ENDPOINT. Silent on failure:
 * a process that cannot reach the desktop keeps the defaults, exactly as one
 * that cannot open the file does. */
static inline void oscfg_ask_desktop(struct oscfg *c) {
    int ch = (int)embk_chan_connect(OSCFG_ENDPOINT);
    if (ch < 0) return;
    char buf[2048];
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
    char b[2048];
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
static inline void oscfg_snapshot(struct oscfg *c) {
    oscfg_defaults(c);
    if (!oscfg_read_from(c, oscfg_path()))
        oscfg_ask_desktop(c);
    oscfg_clamp(c);
}
static inline void oscfg_load(struct oscfg *c) {
    oscfg_snapshot(c);
    /* LOADING THE PREFERENCES MEANS WEARING THEM, for the one preference that
     * is invisible until something formats a date. An app that reads the
     * config to find the accent colour also gets the right clock, and no app
     * has to remember a second call it would only notice missing at 1am. */
    oscfg_apply_tz(c);
}

static inline void oscfg_parse(struct oscfg *c, char *buf) {
    for (char *p = buf; *p; ) {
        char key[32]; int val = 0;
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        if (line[0] == '#' || !line[0]) continue;
        /* One monitor of the saved arrangement: id enabled primary w h x y.
         * The id is unsigned, so it is read here and not by the %d below. */
        if (!strncmp(line, "display_output ", 15)) {
            struct display_saved *l = &c->display_layout;
            unsigned id; int e, pr, w, h, x, y;
            if (sscanf(line + 15, "%u %d %d %d %d %d %d", &id, &e, &pr, &w, &h, &x, &y) == 7 &&
                l->count >= 0 && l->count < (int)EMBK_DISPLAY_MAX_OUTPUTS) {
                l->out[l->count].id = id; l->out[l->count].enabled = e; l->out[l->count].primary = pr;
                l->out[l->count].width = w; l->out[l->count].height = h; l->out[l->count].x = x; l->out[l->count].y = y;
                l->count++;
            } else l->count = -1;                     /* malformed: oscfg_clamp drops the whole arrangement */
            continue;
        }
        if (sscanf(line, "%31s %d", key, &val) != 2) continue;
        #define OSCFG_PARSE(name, value, lo, hi) if (!strcmp(key, #name)) { c->name=val; continue; }
        OSCFG_CONTOUR_FIELDS(OSCFG_PARSE)
        #undef OSCFG_PARSE
        if(!strcmp(key,"idle_dim_seconds"))c->idle_dim_seconds=val;
        else if(!strcmp(key,"idle_lock_seconds"))c->idle_lock_seconds=val;
        else if(!strcmp(key,"lid_lock"))c->lid_lock=val;
        else if(!strcmp(key,"power_profile"))c->power_profile=val;
        else if(!strcmp(key,"display_width"))c->display_width=val;
        else if(!strcmp(key,"display_height"))c->display_height=val;
        else if(!strcmp(key,"display_mirror")){memset(&c->display_layout,0,sizeof c->display_layout);c->display_layout.mirror=val;}
        else if(!strcmp(key,"volume"))c->volume=val;
        else if(!strcmp(key,"muted"))c->muted=val;
        else if (!strcmp(key, "accent"))    c->accent    = val;
        else if (!strcmp(key, "dark"))      c->dark      = val;
        else if (!strcmp(key, "dock_size")) c->dock_size = val;
        else if (!strcmp(key, "dock_dots")) c->dock_dots = val;
        else if (!strcmp(key, "keymap")) c->keymap = (val >= 0 && val < OSCFG_KEYMAPS) ? val : 0;
        else if (!strcmp(key, "ui_scale"))  c->ui_scale  = val;
        else if (!strcmp(key, "wallpaper")) c->wallpaper = val;
        else if (!strcmp(key, "timezone"))  c->tz        = val;
        else if (!strcmp(key, "bar_24h"))     c->bar_24h     = val;
        else if (!strcmp(key, "bar_seconds")) c->bar_seconds = val;
        else if (!strcmp(key, "bar_date"))    c->bar_date    = val;
        else if (!strcmp(key, "bar_cpu"))     c->bar_cpu     = val;
    }
}

/* Atomic replacement with file and parent-directory sync; a failed save is
 * reported to Settings. The old or complete new snapshot remains readable. */
/* The whole preference set as text. One serialiser, used by the file and by the
 * channel below: two encodings of the same thing is how they drift apart. */
static inline int oscfg_format(const struct oscfg *c, char *out, int cap) {
    int n = snprintf(out, (size_t)cap,
                     "# EmbLink preferences -- written by Settings, editable by hand.\n"
                     "accent %d\n" "dark %d\n" "dock_size %d\n" "dock_dots %d\n"
                     "ui_scale %d\n" "keymap %d\n" "wallpaper %d\n" "timezone %d\n"
                     "bar_24h %d\n" "bar_seconds %d\n" "bar_date %d\n" "bar_cpu %d\n" "volume %d\n" "muted %d\n" "idle_dim_seconds %d\n" "idle_lock_seconds %d\n" "lid_lock %d\n" "power_profile %d\n",
                     c->accent, c->dark, c->dock_size, c->dock_dots, c->ui_scale,
                     c->keymap, c->wallpaper, c->tz,
                     c->bar_24h, c->bar_seconds, c->bar_date, c->bar_cpu,c->volume,c->muted,c->idle_dim_seconds,c->idle_lock_seconds,c->lid_lock,c->power_profile);
    if(n<0 || n>=cap)return -1;
    #define OSCFG_PRINT(name, value, lo, hi) { int add=snprintf(out+n,(size_t)(cap-n),#name " %d\n",c->name); if(add<0 || add>=cap-n)return -1; n+=add; }
    OSCFG_CONTOUR_FIELDS(OSCFG_PRINT)
    #undef OSCFG_PRINT
    int added=snprintf(out+n,(size_t)(cap-n),"display_width %d\ndisplay_height %d\n",c->display_width,c->display_height);
    if(added<0 || added>=cap-n)return -1;
    n+=added;
    const struct display_saved *l=&c->display_layout;
    if(l->count>0) {
        added=snprintf(out+n,(size_t)(cap-n),"display_mirror %d\n",l->mirror);
        if(added<0 || added>=cap-n)return -1;
        n+=added;
        for(int i=0;i<l->count && i<(int)EMBK_DISPLAY_MAX_OUTPUTS;i++) {
            added=snprintf(out+n,(size_t)(cap-n),"display_output %u %d %d %d %d %d %d\n",(unsigned)l->out[i].id,
                           l->out[i].enabled,l->out[i].primary,l->out[i].width,l->out[i].height,l->out[i].x,l->out[i].y);
            if(added<0 || added>=cap-n)return -1;
            n+=added;
        }
    }
    return n;
}

static inline int oscfg_write_to(const struct oscfg *c, const char *path) {
    char out[2048],temporary[240];static unsigned sequence;
    int n=oscfg_format(c,out,sizeof out);
    if(n<0 || n>=(int)sizeof out)return -1;
    snprintf(temporary,sizeof temporary,"%s.pending-%u-%u",path,(unsigned)embk_getpid(),++sequence);
    int fd=(int)embk_open(temporary,EMBK_O_WRONLY|EMBK_O_CREAT|EMBK_O_EXCL,0600);
    if(fd<0)return -1;
    int done=0,rc=0;
    while(done<n) {
        int64_t wrote=embk_write(fd,out+done,(size_t)(n-done));
        if(wrote<=0){rc=-1;break;}done+=(int)wrote;
    }
    if(!rc && embk_fsync(fd))rc=-1;
    if(embk_close(fd))rc=-1;
    if(!rc)rc=(int)embk_syscall2(EMBK_SYS_rename,(int64_t)(uintptr_t)temporary,(int64_t)(uintptr_t)path);
    if(!rc) {
        char parent[240];snprintf(parent,sizeof parent,"%s",path);
        char *slash=strrchr(parent,'/');
        if(!slash)snprintf(parent,sizeof parent,".");else if(slash==parent)slash[1]=0;else *slash=0;
        fd=(int)embk_open(parent,EMBK_O_RDONLY,0);
        if(fd<0)rc=-1;
        else {if(embk_fsync(fd))rc=-1;if(embk_close(fd))rc=-1;}
    }
    if(rc)embk_unlink(temporary);
    return rc? -1:0;
}

static inline int oscfg_save(const struct oscfg *c) {
    return oscfg_write_to(c, oscfg_path());
}

/* Hand one connected peer the preferences and be done. Called by the desktop's
 * little server thread; see OSCFG_ENDPOINT above. */
static inline int oscfg_serve_one(int ch, const struct oscfg *c) {
    char out[2048];
    int n = oscfg_format(c, out, (int)sizeof out);
    if(n<0)return -1;
    return embk_chan_send(ch, out, (unsigned)n, 0, 0, 0);
}

#endif /* _EMBLINK_OSCFG_H_ */
