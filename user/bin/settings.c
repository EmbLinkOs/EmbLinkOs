/* user/bin/settings.c -- System Settings.
 *
 * The rule this app is built to: every control here CHANGES SOMETHING. A
 * settings window full of switches that only move themselves is worse than no
 * settings window, because it teaches the user that the OS lies to them. So
 * the panes are short and each one is real:
 *
 *   Appearance      accent + light/dark, applied to THIS window the instant
 *                   you pick it, written to /data/settings.conf, and worn by
 *                   every application at launch (em_app_run reads the same
 *                   file -- see user/lib/oscfg.h).
 *   Desktop & Dock  dock icon size and the running indicator, which the
 *                   desktop re-reads live: the dock changes under you.
 *   System          measured, not typed: uptime, resolution, live process
 *                   count, all read from the kernel each second.
 *   About           what this actually is.
 *
 * There is no "Apply" button. A preference that needs confirming is a
 * preference the program was not confident enough to show you.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "oscfg.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

enum { PANE_APPEARANCE = 0, PANE_DESKTOP, PANE_SYSTEM, PANE_ABOUT, PANE_N };

static const char *g_pane_name[PANE_N] = { "Appearance", "Desktop & Dock", "System", "About" };
static const int   g_pane_icon[PANE_N] = { IconStar, IconGrid, IconBolt, IconInfo };

static int   g_pane = PANE_APPEARANCE;
static float g_scroll = 0;
static struct oscfg g_cfg;
static bool  g_loaded = false;
static char  g_saved[64] = "";       /* the quiet confirmation line */

/* live system readings, refreshed on the runtime's 1s tick */
static char g_uptime[32] = "--";
static char g_res[32]    = "--";
static char g_procs[32]  = "--";

static void apply_now(void) {
    ui_theme_use_dark(g_cfg.dark != 0);
    const struct oscfg_accent *a = &oscfg_accents[g_cfg.accent];
    ui_theme_set_accent((struct color){ a->r, a->g, a->b, 1.0f });
    em_request_frame();
}

/* Every setter goes through here: change, apply, persist. Splitting those
 * three would eventually let one of them be forgotten. */
static void commit(void) {
    apply_now();
    if (oscfg_save(&g_cfg) == 0) snprintf(g_saved, sizeof g_saved, "Saved");
    else snprintf(g_saved, sizeof g_saved, "Could not write %s", OSCFG_PATH);
}

static void sample_system(void) {
    uint64_t ms = embk_uptime_ms();
    unsigned long s = (unsigned long)(ms / 1000);
    if (s >= 3600) snprintf(g_uptime, sizeof g_uptime, "%lu h %lu min", s / 3600, (s % 3600) / 60);
    else           snprintf(g_uptime, sizeof g_uptime, "%lu min %lu s", s / 60, s % 60);

    uint32_t w = 0, h = 0;
    embk_screen_size(&w, &h);
    snprintf(g_res, sizeof g_res, "%u x %u", (unsigned)w, (unsigned)h);

    struct embk_proc_info p[64];
    int n = embk_proc_list(p, 64);
    snprintf(g_procs, sizeof g_procs, "%d", n < 0 ? 0 : n);
}

/* ---- shared row shapes ------------------------------------------------- */

/* A settings row is a NAME, an explanation of what it does, and then the
 * control -- in that order, with the control trailing so a column of rows
 * shares one right edge and the eye can run straight down it. The explanation
 * is not optional decoration: a switch labelled only "Running indicator" makes
 * the reader guess, and guessing is what settings windows are infamous for. */
static void setting_label(const char *title, const char *why) {
    VStack(.spacing = 1, .grow = 1, .align = Leading) {
        Text(title).body();
        if (why && why[0]) Text(why).caption().tertiary();
    }
}

static void pane_appearance(void) {
    const struct ui_theme *t = ui_theme();
    static const char *modes[] = { "Light", "Dark" };
    int dark = g_cfg.dark ? 1 : 0;

    Section("Theme") {
        HStack(.spacing = 16, .align = Center, .py = 8, .grow = 1) {
            setting_label("Appearance", "Light or dark ground for every window.");
            Segmented(modes, 2, &dark);
        }
        if (dark != (g_cfg.dark ? 1 : 0)) { g_cfg.dark = dark; commit(); }
    }

    Section("Accent") {
        HStack(.spacing = 16, .align = Center, .py = 8, .grow = 1) {
            setting_label("Highlight colour",
                          "Selection, focus and active controls -- and nothing else.");
        }
        /* The swatches are the control. A dropdown listing colour NAMES would
         * make you imagine the colour; showing them lets you choose one. */
        HStack(.spacing = 10, .align = Center, .py = 4) {
            for (int i = 0; i < OSCFG_ACCENTS; i++) {
                const struct oscfg_accent *a = &oscfg_accents[i];
                bool on = (g_cfg.accent == i);
                VStack(.spacing = 6, .align = Center, .width = 74) {
                    if (Button(on ? "\xE2\x9C\x93" : " ")
                            .bg((Color){ a->r, a->g, a->b, 1.f })
                            .color(on ? (Color){ 1,1,1,1 } : (Color){ a->r, a->g, a->b, 1.f })
                            .frame(46, 34).corner(9)
                            .id(a->name).clicked()) {
                        g_cfg.accent = i; commit();
                    }
                    Text(a->name).caption().color(on ? t->accent : t->text_secondary);
                }
            }
        }
    }
}

static void pane_desktop(void) {
    Section("Dock") {
        HStack(.spacing = 16, .align = Center, .py = 8, .grow = 1) {
            setting_label("Icon size", "How large the apps in the dock are drawn.");
            char v[16]; snprintf(v, sizeof v, "%d px", g_cfg.dock_size);
            Text(v).caption().secondary();
        }
        /* A slider, because size is a quantity you tune by eye, not a value you
         * type. It commits on release rather than per-pixel: writing the file
         * on every frame of a drag would be a hundred writes for one decision. */
        float f = (float)(g_cfg.dock_size - 28) / 32.0f;
        static bool dragging = false;
        Slider(&f);
        int want = 28 + (int)(f * 32.0f + 0.5f);
        if (want != g_cfg.dock_size) { g_cfg.dock_size = want; dragging = true; apply_now(); }
        else if (dragging) { dragging = false; commit(); }

        bool dots = g_cfg.dock_dots != 0, was = dots;
        HStack(.spacing = 16, .align = Center, .py = 8, .grow = 1) {
            setting_label("Running indicator",
                          "A dot under an app that is open, so the dock tells the truth.");
            Toggle("", &dots);
        }
        if (dots != was) { g_cfg.dock_dots = dots ? 1 : 0; commit(); }
    }
    Section("Desktop") {
        HStack(.spacing = 16, .align = Center, .py = 8, .grow = 1) {
            setting_label("Icon arrangement",
                          "Icons stay wherever you drop them; the desktop menu tidies them.");
            Text("Free").caption().secondary();
        }
    }
}

static void pane_system(void) {
    Section("This machine") {
        ListRow(IconBolt, "Uptime",        g_uptime);
        ListRow(IconGrid, "Display",       g_res);
        ListRow(IconList, "Live processes", g_procs);
    }
    Section("Software") {
        ListRow(IconInfo, "System",   "EmbLink OS");
        ListRow(IconInfo, "Kernel",   "EmbLink, x86-64");
        ListRow(IconInfo, "Toolkit",  "EmUI");
        ListRow(IconInfo, "Shell",    "EmbLink shell");
    }
}

static void pane_about(void) {
    VStack(.spacing = 10, .align = Leading, .py = 10) {
        Text("EmbLink OS").title();
        Text("An operating system written from the boot sector up: its own kernel, "
             "filesystem, network stack, TLS, package manager, compiler toolchain "
             "and this user interface.").body().secondary();
        Divider();
        Text("Preferences are stored in " OSCFG_PATH " as plain text. "
             "Editing that file by hand is a supported way to change them.")
            .caption().tertiary();
    }
}

static void app(void) {
    if (!g_loaded) { oscfg_load(&g_cfg); g_loaded = true; apply_now(); }
    sample_system();

    Window("Settings") {
        AppBar("Settings") {
            if (g_saved[0]) Text(g_saved).caption().tertiary();
        }
        Split(206) {
            SidebarPane() {
                const struct ui_theme *t = ui_theme();
                for (int i = 0; i < PANE_N; i++) {
                    bool on = (g_pane == i);
                    HStack(.spacing = 9, .align = Center, .px = 8, .py = 5, .corner = 7,
                           .grow = 1,
                           .background = on ? (Color){ .r=.20f, .g=.22f, .b=.30f, .a=1.f }
                                            : (Color){ 0, 0, 0, 0 }) {
                        Icon(g_pane_icon[i]).color(on ? t->accent : t->text_secondary);
                        if (Button(g_pane_name[i]).ghost().color(on ? t->accent : t->text)
                                .grow().leading().clicked()) { g_pane = i; g_scroll = 0; g_saved[0] = 0; }
                    }
                }
                Spacer();
                Text("EmbLink OS").caption().tertiary();
            }
            ContentPane(.padding = 0) {
                ScrollView(&g_scroll, 470) {
                    VStack(.spacing = 6, .align = Fill, .padding = 20) {
                        Text(g_pane_name[g_pane]).heading();
                        switch (g_pane) {
                            case PANE_APPEARANCE: pane_appearance(); break;
                            case PANE_DESKTOP:    pane_desktop();    break;
                            case PANE_SYSTEM:     pane_system();     break;
                            default:              pane_about();      break;
                        }
                    }
                }
            }
        }
    }
}

EM_APPLICATION {
    .title      = "Settings",
    .size       = { 860, 600 },
    .theme      = Dark,
    .chrome     = Chromeless,
    .resize     = Resizable,
    .refresh_ms = 1000,     /* the System pane shows live readings */
    .view       = app,
};
