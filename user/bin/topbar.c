/* topbar.c -- a dynamic, Apple-modern menu bar.
 *
 * A floating GLASS (Acrylic) window shaped like a menu bar:
 *   left  : a leading logo + a File/Edit/View menu bar (V6 menus)
 *   middle: a DRAG HANDLE -- press-and-drag the empty middle to move the bar
 *   right : a DOCK of status chips you drag to reorder / pull out to remove,
 *           a clock, and a PIN button that cycles it between screen anchors.
 *
 * The bar is not app chrome -- it's a chromeless glass strip. The EM_APPLICATION
 * runtime binds the window so DragHandle moves it and the pin snaps it. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define BAR_W 880

/* status-chip ids (icon codepoints); the user reorders / removes these live */
static int g_items[8] = { IconStar, IconBolt, IconGear, IconHeart };
static int g_n        = 4;
static int g_pin      = 1;   /* 0 free, 1 top-center, 2 top-left, 3 top-right */

static void chip(int id) { Icon(id).secondary(); }

/* Snap the bar to a screen anchor (the pin control cycles these). */
static void snap_to(int anchor) {
    uint32_t sw = 1024, sh = 768;
    embk_screen_size(&sw, &sh);
    int32_t x, y = 6;
    switch (anchor) {
        case 1: x = ((int)sw - BAR_W) / 2; break;   /* top center */
        case 2: x = 10;                    break;   /* top left   */
        case 3: x = (int)sw - BAR_W - 10;  break;   /* top right  */
        default: return;                            /* 0 = free (no snap) */
    }
    em_window_move_to(x, y);
}

/* Thin like a real menu bar: the strip is chrome, not a panel. */
#define BAR_H 32

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
    if (first) { first = 0; snap_to(1); }

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
        em_window_blur_rect(0, 0, (int)em_viewport_width(), BAR_H);
        HStack(.height = BAR_H, .align = Center, .spacing = 8, .px = 10,
               .background = { .r=.05f, .g=.055f, .b=.075f, .a=.72f },
               .corner = 16, .border = 1) {
            /* The leading mark IS the Apps launcher button (like a Start menu).
             * Real art rather than a font glyph, but drawn as a STENCIL in the
             * accent colour: it keeps the bar's controls one coherent palette
             * and follows a theme change, which baked-in art could not. */
            if (ImageButtonTinted("/system/images/launcher.eic", 26, t->accent))
                request_apps();
            MenuBar() {
                /* Where the bar sits belongs in the system menu, not on the bar.
                 * A visible "pinned"/"free" text button was the one control
                 * shouting its own implementation at the user; a menu bar should
                 * carry menus and status, nothing else. */
                Menu("EmbLink") {
                    MenuItem("About EmbLink");
                    MenuSeparator();
                    if (MenuItem("Position: Left"))   { g_pin = 2; snap_to(2); }
                    if (MenuItem("Position: Center")) { g_pin = 1; snap_to(1); }
                    if (MenuItem("Position: Right"))  { g_pin = 3; snap_to(3); }
                    if (MenuItem("Position: Free"))   { g_pin = 0; }
                    MenuSeparator();
                    if (MenuItem("Quit")) exit(0);
                }
                Menu("File") { MenuItem("New"); MenuItem("Open"); }
                Menu("Edit") { MenuItem("Undo"); MenuItem("Redo"); }
                Menu("View") { MenuItem("Zoom In"); MenuItem("Zoom Out"); }
            }

            /* the empty middle drags the whole bar -- give it a real height so
             * it's grabbable (an intrinsic-height empty strip collapses to 0px
             * and the press falls through, so the drag never starts). */
            DragHandle(.grow = 1, .height = 22) { }

            /* status items, then the clock -- the trailing order a menu bar has */
            Dock(g_items, &g_n, chip);
            Text(bar_clock()).caption();
        }
        Spacer();   /* transparent canvas below the bar -- dropdown room */
    }
}

EM_APPLICATION {
    .title    = "TopBar",
    .size     = { BAR_W, BAR_H },
    .theme    = Dark,
    .chrome   = Chromeless,
    .material = Translucent,      /* thin transparent bar; grows for dropdowns */
    .view     = bar,
};
