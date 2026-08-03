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

static void bar(void) {
    const struct ui_theme *t = ui_theme();

    /* park at the top-center the first time we're drawn */
    static int first = 1;
    if (first) { first = 0; snap_to(1); }

    HStack(.grow = 1, .align = Center, .spacing = 10, .px = 12) {
        Icon(IconBolt).color(t->text);
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

        /* the empty middle drags the whole bar */
        DragHandle(.grow = 1) { }

        /* draggable status chips: reorder / pull one down to remove */
        Dock(g_items, &g_n, chip);
        Text("9:41").bold();

        /* pin: cycle free -> center -> left -> right; the glyph shows pinned */
        if (Button(g_pin ? "pinned" : "free").ghost().clicked()) {
            g_pin = (g_pin + 1) % 4;
            snap_to(g_pin);
        }
    }
}

EM_APPLICATION {
    .title    = "TopBar",
    .size     = { BAR_W, 40 },
    .theme    = Dark,
    .chrome   = Chromeless,
    .material = Acrylic,          /* frosted glass */
    .view     = bar,
};
