/* topbar.c -- the system menu bar.
 *
 * Modelled on the Mac menu bar, which is a very particular thing: it SPANS the
 * display, sits flush in the top-left corner with no margin and no rounding,
 * and is thin. It is part of the screen rather than an object on it. It was
 * previously a floating rounded 880px pill you could drag around and pin to
 * anchors -- which is a nice widget, and reads as a widget, which is exactly
 * what a menu bar must not read as.
 *
 * So: full width, flush, 26px, no corners, no drag, no pin. Left is the
 * launcher mark then the menus, with the owning app's name BOLD (that weight
 * difference is how a menu bar says whose menus these are). Right is bare
 * status glyphs and the clock -- glyphs, not chips: boxing each status item
 * turned the bar into a row of widgets.
 *
 * The window is TRANSLUCENT and only the top strip paints; the rest is
 * transparent canvas the dropdowns render into.
 *
 * The strip itself paints NOTHING now -- no fill, no frost. The menu bar is
 * just its text and glyphs sitting directly on the wallpaper, which is the
 * most confident version of this and the one that makes the desktop feel like
 * a single surface rather than a stack of panels.
 *
 * With no background, legibility is the wallpaper's problem, so the bar ASKS:
 * embk_screen_luma samples what is composed just below the strip and the bar
 * inks itself dark over a light wallpaper and light over a dark one. That is
 * how a transparent bar stays readable without reintroducing the panel it was
 * trying not to be. Sampled about twice a second -- a wallpaper does not
 * change at frame rate, and reading pixels is not free. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "embk.h"
#include "emnotify.h"   /* About speaks through the notifier */
#include "ui.h"
#include "em.h"
#include "theme.h"

/* Which machine this is, decided where it is actually known -- at compile
 * time. A binary cannot be wrong about its own architecture. */
#if defined(__aarch64__)
#define ARCH_NAME "aarch64"
#elif defined(__x86_64__)
#define ARCH_NAME "x86-64"
#else
#define ARCH_NAME "unknown"
#endif

#define BAR_W 1024   /* replaced at startup by the real display width */
/* Thin like a real menu bar: the strip is chrome, not a panel. */
#define BAR_H 26

/* Ink for everything in the strip: dark over a light wallpaper, light over a
 * dark one. Sampled from the band immediately BELOW the bar -- the bar's own
 * pixels are in the framebuffer too, so sampling its own row would measure its
 * own text. Hysteresis around the midpoint: a wallpaper that sits near the
 * threshold must not make the bar flicker between inks. */
static Color g_ink = { .r=.90f, .g=.91f, .b=.93f, .a=1.f };
static int   g_ink_dark = 0;
static uint64_t g_ink_next = 0;

static void bar_ink_update(void) {
    uint64_t now = em_now_ms();
    /* Not before the desktop exists. The very first sample used to land during
     * boot, while the screen still held the kernel console, and latch an ink
     * chosen from white-on-black text that no user ever saw. */
    if (now < 2500) return;
    if (now < g_ink_next) return;
    g_ink_next = now + 500;
    int l = embk_screen_luma(0, BAR_H, (int)em_viewport_width(), 6);
    if (l < 0) return;
    if (!g_ink_dark && l > 150)      g_ink_dark = 1;
    else if (g_ink_dark && l < 120)  g_ink_dark = 0;
    g_ink = g_ink_dark ? (Color){ .r=.08f, .g=.08f, .b=.09f, .a=1.f }
                       : (Color){ .r=.90f, .g=.91f, .b=.93f, .a=1.f };
}

/* status-chip ids (icon codepoints); the user reorders / removes these live */
/* WHAT THE MACHINE IS DOING, and it is measured.
 *
 * What used to trail this bar was four glyphs -- a star, a lightning bolt, a
 * gear and a heart -- that were not connected to anything. They were there
 * because a menu bar has status icons on its right, which is a description of
 * someone else's menu bar, not a reason. (The clock beside them had the same
 * problem once: it read a hard-coded 9:41, Apple's marketing time, until
 * somebody noticed the OS could just say what time it is.)
 *
 * So this bar reports the one thing this OS is in a position to know better
 * than it knows anything else: how much of the machine is being used, right
 * now, by the user's own programs.
 *
 * PER CORE, like every `top` ever written: cpu_ns is time spent EXECUTING, so
 * the delta over a wall-clock second is the fraction of ONE core, and four
 * busy cores read 400%. That is a real number with a real unit, and it needs
 * no idea of how many cores the machine has -- which userspace has no way to
 * ask for anyway, and which is not worth inventing a lie about.
 *
 * KERNEL THREADS ARE EXCLUDED. They are the machine's own overhead, not the
 * session's work, and the idle threads are kernel threads -- counting those
 * would report a quiet machine as a busy one.
 *
 * The one thing it cannot see: time charged to a process that EXITED between
 * two samples leaves the total, which would make the delta negative. The
 * sample is skipped rather than reported as a wild number, so the reading
 * holds its last value for one second after something quits. 64 rows is the
 * kernel's whole table (MAX_PROCESSES), so nothing is missed for being past
 * the end of the buffer. */
static int   g_cpu_pct = 0;
static uint64_t g_cpu_last_ns = 0, g_cpu_last_ms = 0;

static void cpu_sample(void) {
    static struct embk_proc_info ps[64];
    int n = embk_proc_list(ps, 64);
    if (n <= 0) return;

    uint64_t busy = 0;
    for (int i = 0; i < n; i++)
        if (!ps[i].is_kthread) busy += ps[i].cpu_ns;

    uint64_t now = embk_uptime_ms();
    if (g_cpu_last_ms && now > g_cpu_last_ms && busy >= g_cpu_last_ns) {
        uint64_t dns = busy - g_cpu_last_ns;
        uint64_t dms = now - g_cpu_last_ms;
        /* ns per ms = fraction of one core x 1e6; x100 for a percentage. */
        g_cpu_pct = (int)(dns / (dms * 10000ull));
        if (g_cpu_pct > 999) g_cpu_pct = 999;
    }
    g_cpu_last_ns = busy;
    g_cpu_last_ms = now;
}

/* LABELLED, and that is not decoration. A bare percentage in the corner of a
 * menu bar is read as a battery by everyone who has ever used a computer --
 * and this OS has no battery driver at all yet, so the one number it can
 * honestly show there had better say what it is. */
static const char *cpu_text(void) {
    static char buf[24];
    snprintf(buf, sizeof buf, "CPU %d%%", g_cpu_pct);
    return buf;
}

/* A 24px rule that fills with the load. Small enough to read as punctuation
 * next to the number rather than as a gauge competing with it -- the number is
 * the reading; this is the shape of the reading, for the glance that does not
 * stop to read.
 *
 * FULL SCALE IS ONE CORE, not the whole machine. Scaled to four, an ordinary
 * desktop (measured: 45% while a terminal and the shell were up) filled three
 * pixels of twenty-four and the meter was a dot -- technically true and
 * useless at a glance. One core is the threshold that actually means
 * something: full means "something is working hard", and a machine with more
 * cores keeps working past it rather than the meter rescaling underfoot. */
static void meter(int pct) {
    float frac = (float)pct / 100.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    Color track = g_ink; track.a = 0.22f;
    HStack(.width = 24, .height = 4, .corner = 2, .background = track,
           .align = Fill, .justify = Leading) {
        /* ALWAYS EMITTED, never conditional, and at least a pixel wide. Two
         * reasons and they point the same way: a bar that rounds a live
         * machine down to nothing reads as "no data" rather than "idle"; and a
         * child that comes and goes is the retained-instance trap this toolkit
         * has paid for twice (em_apply_box has the story). A box that is
         * always there cannot be reused as something else. */
        float w = frac * 24.0f;
        if (w < 1.0f) w = 1.0f;
        HStack(.width = w, .height = 4, .corner = 2, .background = g_ink) {}
    }
}


/* Live date + time. The bar used to read a hard-coded "9:41" -- Apple's
 * marketing time -- which is exactly the kind of decorative lie the rest of the
 * shell avoids. The OS can report the real clock, so it does. */
static const char *bar_clock(void) {
    static char buf[48];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    /* Standard conversions only: newlib's strftime does not implement the GNU
     * "%-d" no-pad flag and stops at it, which silently truncated the whole
     * string to just the weekday. */
    if (tm) snprintf(buf, sizeof buf, "%s %d %s  %02d:%02d",
                     (const char *[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[tm->tm_wday % 7],
                     tm->tm_mday,
                     (const char *[]){"Jan","Feb","Mar","Apr","May","Jun",
                                      "Jul","Aug","Sep","Oct","Nov","Dec"}[tm->tm_mon % 12],
                     tm->tm_hour, tm->tm_min);
    else    snprintf(buf, sizeof buf, "--:--");
    return buf;
}

/* Ask the desktop to open the Apps launcher. The launcher renders on the desktop
 * (same program as the dock, so apps can be dragged into it); the top bar is a
 * separate process, so it signals by CONNECTING to the desktop's IPC channel in
 * /run (the desktop listens there and opens the grid). The connect itself is the
 * signal -- no payload -- so we close immediately. */
static void request_apps(void) {
    embk_puts(1, "TopBar: launcher button pressed\n");
    int ch = (int)embk_chan_connect("/run/emlink.desktop");
    if (ch >= 0) {
        embk_chan_close(ch);
        return;
    }
    /* SAY SO. This used to fail silently, and a launcher button that does
     * nothing gives the user no way to tell a missing desktop listener from a
     * missing /run from a dead click -- aarch64 had the second for its whole
     * life and nothing on screen or in the log said which. */
    char b[96];
    snprintf(b, sizeof b, "TopBar: could not reach the desktop to open the launcher (%d)\n", ch);
    embk_puts(1, b);
}

/* THE WINDOWS THAT EXIST, sampled on the bar's own beat.
 *
 * The bar could not name the focused application before -- nothing could. A
 * process in this system is named by handle and stores no path, so the only
 * human-readable name an application has is the title of its window, and until
 * embk_win_list there was no way to ask the compositor for it. In its place
 * the bar carried three menus (File, Edit, View) that belonged to no app and
 * did nothing, because a menu bar is SUPPOSED to say whose menus these are and
 * this one had no way to find out.
 *
 * Now it says the truth: the focused application's name, and a menu of
 * everything open that switches to it. */
#define WINS_MAX 16
static struct embk_win_info g_wins[WINS_MAX];
static int      g_win_n;
static char     g_focus_name[32];
static uint64_t g_wins_next;

static void wins_sample(void) {
    uint64_t now = em_now_ms();
    if (now < g_wins_next) return;
    g_wins_next = now + 700;               /* windows do not open at frame rate */

    int n = embk_win_list(g_wins, WINS_MAX);
    g_win_n = n < 0 ? 0 : n;
    g_focus_name[0] = 0;
    for (int i = 0; i < g_win_n; i++)
        if (g_wins[i].flags & EMBK_WIN_FOCUSED) {
            snprintf(g_focus_name, sizeof g_focus_name, "%s", g_wins[i].title);
            break;
        }
}

static void bar(void) {
    const struct ui_theme *t = ui_theme();

    /* park at the top-center the first time we're drawn */
    static int first = 1;
    if (first) { first = 0; em_window_move_to(0, 0); }   /* flush, like Mac's */
    bar_ink_update();
    cpu_sample();      /* once per frame == once per refresh_ms == once a second */
    wins_sample();     /* who is in front, and what else is open */

    /* The window is TRANSLUCENT (per-pixel transparent, no blur) and grows tall
     * only while a menu is open. So the view fills the whole window, but only
     * the top strip paints a bar -- the rest stays transparent (revealing the
     * desktop) and holds the room a dropdown needs to render OUTSIDE the bar. */
    VStack(.width = em_viewport_width(), .height = em_viewport_height(),
           .align = Fill, .spacing = 0) {
        /* Tighter spacing and a fuller round than a panel would use: at 32px the
         * strip reads as chrome the content sits under, not a box on top of it. */
        /* REAL glass: the compositor frosts the desktop behind the strip's
         * rect (and only the strip -- the dropdown canvas below must stay a
         * sharp view of the desktop). The fill drops to a tint so the frost
         * reads through: the same material as the dock and the launcher. */
        /* No blur rect declared: frosting the strip would be a material, and
         * the point is that there is no material. No corner radius and no
         * border either -- the bar meets the screen edges, so an outline would
         * only draw a box around the top of the display. */
        HStack(.height = BAR_H, .align = Center, .spacing = 6, .px = 8) {
            /* The leading mark IS the Apps launcher button (like a Start menu).
             * Real art rather than a font glyph, but drawn as a STENCIL in the
             * accent colour: it keeps the bar's controls one coherent palette
             * and follows a theme change, which baked-in art could not. */
            if (ImageButtonTinted("/system/images/launcher.eic", 17, t->accent))
                request_apps();
            MenuBar() {
                /* Where the bar sits belongs in the system menu, not on the bar.
                 * A visible "pinned"/"free" text button was the one control
                 * shouting its own implementation at the user; a menu bar should
                 * carry menus and status, nothing else. */
                /* BOLD: the leading menu names the application these menus
                 * belong to, and weight is how a menu bar says so. */
                Menu("EmbLink", .font = BodyBold, .color = g_ink) {
                    /* A DEAD MENU ITEM IS A LIE, and this one had been sitting
                     * here doing nothing. It says something true now: the name
                     * the system calls itself and the machine it is running
                     * on, delivered the way any program speaks from outside its
                     * own window. */
                    if (MenuItem("About EmbLink")) {
                        char body[96];
                        uint32_t sw = 0, sh = 0;
                        embk_screen_size(&sw, &sh);
                        snprintf(body, sizeof body, "%s \xc2\xb7 %ux%u \xc2\xb7 up %llu min",
                                 ARCH_NAME, (unsigned)sw, (unsigned)sh,
                                 (unsigned long long)(embk_uptime_ms() / 60000ull));
                        embk_notify("EmbLink OS", body);
                    }
                    MenuSeparator();
                    /* LOG OUT ends the session -- every process in it, this
                     * bar included -- and init shows the login screen. The
                     * name is the kernel's record of whose session this is,
                     * not $USER. */
                    static char logout_label[48];
                    if (!logout_label[0]) {
                        struct embk_session_info si;
                        if (embk_session_info(&si) == 0 && si.user[0])
                            snprintf(logout_label, sizeof logout_label, "Log Out %s", si.user);
                        else
                            snprintf(logout_label, sizeof logout_label, "Log Out");
                    }
                    if (MenuItem(logout_label)) embk_session_end(0);
                    MenuSeparator();
                    /* RESTART AND SHUT DOWN. Until these existed the only way
                     * to stop an EmbLink machine was to cut its power -- the
                     * kernel could always do it (power_transition) and nothing
                     * in userspace could ask. Neither returns.
                     *
                     * No confirmation dialog, deliberately: closing a window
                     * now ASKS each application to go, and a shutdown does the
                     * same thing to all of them at once, so the work that a
                     * confirmation protects is already protected. */
                    if (MenuItem("Restart"))   embk_power_restart();
                    if (MenuItem("Shut Down")) embk_power_off();
                }
                /* THE FOCUSED APPLICATION, and a way to reach the others.
                 * This is what a menu bar is for -- saying whose window you
                 * are looking at -- and it is the first time this OS could
                 * answer that question at all (embk_win_list). The name is
                 * BOLD for the same reason the system menu is: weight is how a
                 * bar says which of these belongs to what you are using.
                 *
                 * Empty when nothing is open, rather than a placeholder: the
                 * desktop is not an application and saying so would be the
                 * same lie the File/Edit/View menus told. */
                if (g_focus_name[0]) {
                    Menu(g_focus_name, .font = BodyBold, .color = g_ink) {
                        for (int i = 0; i < g_win_n; i++) {
                            static char label[WINS_MAX][40];
                            snprintf(label[i], sizeof label[i], "%s%s",
                                     g_wins[i].title,
                                     (g_wins[i].flags & EMBK_WIN_MINIMIZED) ? "  (hidden)" : "");
                            if (MenuItem(label[i])) embk_win_raise(g_wins[i].pid);
                        }
                    }
                }

                /* AND NOTHING ELSE. There were three more menus here --
                 * File (New, Open), Edit (Undo, Redo), View (Zoom In, Zoom
                 * Out) -- twelve words that did nothing at all. They were a
                 * costume: a Mac menu bar carries the FOCUSED APP's menus, so
                 * this one grew a set of app-shaped menus with no app behind
                 * them. An OS that has not yet decided where an application's
                 * menus live is better off saying nothing than miming it.
                 *
                 * What is left is the one menu that is really the system's:
                 * about, log out, quit. */
            }

            Spacer();   /* the menus sit left, everything else trails right */

            /* status glyphs, then the clock -- the trailing order a menu bar
             * has. Wider spacing than a toolbar: these are separate readings,
             * not a group of related controls. */
            /* The readout, then the clock -- the trailing order a status
             * strip has, with the number that changes on the inside so the
             * clock keeps the corner it has always had. */
            meter(g_cpu_pct);
            Text(cpu_text()).caption().color(g_ink);
            HStack(.width = 10) {}                 /* two readings, not a group */
            Text(bar_clock()).caption().color(g_ink);
        }
        Spacer();   /* transparent canvas below the bar -- dropdown room */
    }
}

/* Not EM_APPLICATION: the bar has to span whatever display it finds, and the
 * macro's spec is fixed at compile time. Same runtime, one line of setup. */
static EmApp em_app_spec_ = {
    .title    = "TopBar",
    .size     = { BAR_W, BAR_H },
    .theme    = Dark,
    .chrome   = Chromeless,
    .material = Translucent,      /* thin transparent bar; grows for dropdowns */
    .refresh_ms = 1000,           /* the clock is live, and the ink re-samples */
    .view     = bar,
};

int main(void) {
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    if (sw) em_app_spec_.size.w = (int)sw;    /* a menu bar spans the display */
    (void)sh;
    return em_app_run(&em_app_spec_);
}
