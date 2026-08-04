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

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define BAR_W 760

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

#define BAR_H 40

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
    (void)t;

    /* park at the top-center the first time we're drawn */
    static int first = 1;
    if (first) { first = 0; snap_to(1); }

    /* The window is TRANSLUCENT (per-pixel transparent, no blur) and grows tall
     * only while a menu is open. So the view fills the whole window, but only
     * the top strip paints a bar -- the rest stays transparent (revealing the
     * desktop) and holds the room a dropdown needs to render OUTSIDE the bar. */
    VStack(.width = em_viewport_width(), .height = em_viewport_height(),
           .align = Fill, .spacing = 0) {
        HStack(.height = BAR_H, .align = Center, .spacing = 10, .px = 12,
               .background = { .r=.03f, .g=.033f, .b=.043f, .a=.92f },
               .corner = 12, .border = 1) {
            /* the leading logo IS the Apps launcher button (like a Start menu) */
            if (IconButton(IconBolt).accent().clicked()) request_apps();
            MenuBar() {
                Menu("EmbLink") {
                    MenuItem("About EmbLink");
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
            DragHandle(.grow = 1, .height = 28) { }

            /* draggable status chips: reorder / pull one down to remove */
            Dock(g_items, &g_n, chip);
            Text("9:41").bold();

            /* pin: cycle free -> center -> left -> right; the glyph shows pinned */
            if (Button(g_pin ? "pinned" : "free").ghost().clicked()) {
                g_pin = (g_pin + 1) % 4;
                snap_to(g_pin);
            }
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
