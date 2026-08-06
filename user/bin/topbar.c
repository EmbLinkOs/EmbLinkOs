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
#include "ui.h"
#include "em.h"
#include "theme.h"

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
static int g_items[8] = { IconStar, IconBolt, IconGear, IconHeart };
static int g_n        = 4;

/* Each status item gets an equal-width slot with the glyph centred in it.
 * Spacing alone does not do this: glyphs have different widths, so a constant
 * gap puts their CENTRES at uneven distances and the row reads as drifting.
 * Fixed slots are what makes a status row look ruled. */
static void chip(int id) {
    HStack(.width = 22, .height = 18, .align = Center, .justify = Center) {
        Icon(id).color(g_ink);
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
    int ch = (int)embk_chan_connect("/run/emlink.desktop");
    if (ch >= 0) embk_chan_close(ch);
}

static void bar(void) {
    const struct ui_theme *t = ui_theme();

    /* park at the top-center the first time we're drawn */
    static int first = 1;
    if (first) { first = 0; em_window_move_to(0, 0); }   /* flush, like Mac's */
    bar_ink_update();

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
                    MenuItem("About EmbLink");
                    MenuSeparator();
                    if (MenuItem("Quit")) exit(0);
                }
                Menu("File", .color = g_ink) { MenuItem("New"); MenuItem("Open"); }
                Menu("Edit", .color = g_ink) { MenuItem("Undo"); MenuItem("Redo"); }
                Menu("View", .color = g_ink) { MenuItem("Zoom In"); MenuItem("Zoom Out"); }
            }

            Spacer();   /* the menus sit left, everything else trails right */

            /* status glyphs, then the clock -- the trailing order a menu bar
             * has. Wider spacing than a toolbar: these are separate readings,
             * not a group of related controls. */
            Dock(g_items, &g_n, chip, .spacing = 8);
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
