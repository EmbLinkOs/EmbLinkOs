/* ui/dsl/em_app.c -- the EmApplication runtime (EmUI V4.1).
 *
 * TARGET-ONLY translation unit (it speaks the EmbLink SDK); linked into
 * libembk.so but never into the host test/showcase builds. One EM_APPLICATION
 * declaration replaces an app's whole main():
 *
 *   - resources: installs the embk file loader and loads the app font
 *   - toolkit bring-up: arenas, theme, ui_init, animation clock
 *   - the window: kernel-chrome or CHROMELESS (then WindowBar dragging is
 *     wired automatically via the registered mover + origin binding)
 *   - the loop with RETAINED updates: a frame is built ONLY on an input edge,
 *     a ui-epoch bump, a nav transition, or when a live animation asked for
 *     it (em_request_frame). Idle frames cost one input poll + sleep.
 *   - presents: dirty-rect normally; full clear+repaint on structural frames
 *   - teardown: the view's own CloseButton (em_window_closed) or ESC. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "oscfg.h"   /* the user's preferences apply to every app */
#include "ui.h"
#include "kit.h"   /* ui_clipboard_provider: the kit needs one handed to it */
#include "em.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"

static int g_app_exit_requested;
static int g_app_exit_code;
static float g_viewport_w;   /* mirrored into em.c via em_set_viewport */
static int   g_blur_rect[4] = {0,0,0,0};   /* window-local frost rect (w<=0 = none) */
static int   g_blur_dirty = 0;
static int   g_app_win = -1;
/* Declare the sub-rect of this window whose BACKDROP should be frosted by the
 * compositor (translucent windows: the opaque strip of a menu bar). Applied on
 * the next loop turn; call once or whenever the strip geometry changes. */
void em_window_blur_rect(int x, int y, int w, int h) {
    if (g_blur_rect[0]==x && g_blur_rect[1]==y && g_blur_rect[2]==w && g_blur_rect[3]==h) return;
    g_blur_rect[0]=x; g_blur_rect[1]=y; g_blur_rect[2]=w; g_blur_rect[3]=h;
    g_blur_dirty = 1;
}
static float g_viewport_h;

void em_app_request_exit(int code) {
    g_app_exit_code = code;
    g_app_exit_requested = 1;
}



/* --- the embk resource loader (whole file -> malloc'd buffer) ------------ */
static uint8_t *emapp_load(const char *path, size_t *out_len) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { embk_close(fd); return 0; }
    for (;;) {
        if (n + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { embk_close(fd); return 0; } }
        int64_t got = embk_read(fd, buf + n, 65536);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    *out_len = n;
    return buf;
}

static void emapp_mover(int win, int32_t x, int32_t y) { embk_win_move(win, x, y); }

/* --- terminal-shaped runtime hooks (V8; see em.h) ------------------------ */
static int  (*g_em_key_hook)(int ch)  = 0;
static void (*g_em_idle_hook)(void)   = 0;
static void (*g_em_post_layout)(void) = 0;
void em_set_post_layout_hook(void (*fn)(void)) { g_em_post_layout = fn; }
void em_set_key_hook(int (*fn)(int ch)) { g_em_key_hook = fn; }
void em_set_idle_hook(void (*fn)(void)) { g_em_idle_hook = fn; }

static int g_refresh_override = -1;
void em_app_set_refresh(int ms) { g_refresh_override = ms; }

/* ===========================================================================
 * TELLING THE SCHEDULER WHAT THIS LOOP IS.
 *
 * An app loop is periodic while it is animating and idle the rest of the time,
 * and the difference matters to the machine. embk_sched_period() lets a thread
 * say "wake me every `pace` ms, I need about `budget` of it" -- which under the
 * deadline policy is the difference between a frame arriving on time under
 * load and arriving whenever a busy machine gets round to it (measured: a
 * 16 ms loop against six busy threads misses ~half its frames without one and
 * none with).
 *
 * TWO THINGS ARE DELIBERATE HERE.
 *
 * IT IS DECLARED ONLY WHILE ANIMATING. The reservation costs no CPU when the
 * thread is not running, but it does cost ADMISSION: the machine refuses
 * declarations past a ceiling, so a desktop where five idle apps each hold a
 * permanent claim is a desktop where the sixth -- the one actually animating --
 * is refused. Held during a run of frames, given back after a stretch of
 * nothing, with hysteresis so a hesitant app does not churn the syscall.
 *
 * THE BUDGET IS MEASURED, NOT GUESSED. Declaring half the period "to be safe"
 * is claiming half a core for a frame that costs two milliseconds, and two
 * such apps would fill the machine between them. So it is the MINIMUM of the
 * recent frames' CPU cost -- CPU, from embk_thread_cpu_ns(), because on a busy
 * machine the clock measures preemption rather than work, and the minimum of
 * several because even CPU accounting has a tick of noise in it. Erring low is
 * the safe direction: a budget that is too small costs this app its urgency
 * for the tail of a frame; one that is too large costs everyone else their
 * admission.
 * ======================================================================== */
#define CAD_HIST      8    /* frames remembered for the budget estimate      */
#define CAD_GAIN      3    /* heat added by a frame                          */
#define CAD_HOT       9    /* heat at which this counts as animating         */
#define CAD_MAX      24    /* ceiling, so a long animation cools down promptly */

/* HOW "ANIMATING" IS DECIDED, and the first version of this got it wrong.
 *
 * The obvious rule is "N frames in a row", and it is wrong for most of the
 * things that actually animate. A loop polling every 10 ms that renders a
 * 30 fps video builds one frame in every three iterations -- never two in a
 * row -- so a consecutive-frames rule sees a video as idle. (Measured: with
 * that rule nothing on the desktop ever declared anything.)
 *
 * So it is a heat counter instead: a frame adds three, an idle iteration
 * subtracts one, and the claim is held while the total is nine or more. That
 * says "frames are arriving at least a third of the time", which is the
 * property that matters, and it costs one integer. The ceiling keeps a long
 * animation from banking so much heat that it holds the claim for a second
 * after it stops. */
#define CAD_COOLOFF 120    /* frames to wait after a declaration is refused */

struct cadence {
    int      period;          /* what is declared now, 0 = nothing          */
    int      budget;
    int      heat;
    int      cooloff;
    int      said;            /* the one-time explanation has been printed  */
    int      n;
    uint64_t cost[CAD_HIST];
};

static void cad_say(struct cadence *c, const char *title, const char *what) {
    if (c->said) return;
    c->said = 1;
    char b[128];
    snprintf(b, sizeof b, "%s: %s\n", title, what);
    embk_puts(1, b);
}

static void cad_frame(struct cadence *c, int pace, uint64_t cost_ms,
                      const char *title) {
    c->cost[c->n % CAD_HIST] = cost_ms;
    if (c->n < 1000000) c->n++;
    c->heat += CAD_GAIN;
    if (c->heat > CAD_MAX) c->heat = CAD_MAX;
    if (c->heat < CAD_HOT) return;

    /* BACKING OFF AFTER A REFUSAL IS NOT POLITENESS, IT IS CORRECTNESS.
     * Retrying every frame means a syscall per frame that scans the thread
     * table and fails -- measured: with the retry unthrottled, this app's best
     * frame interval went from 20 ms to 45 ms. A refused declaration made the
     * app slower than not asking at all. */
    if (c->cooloff > 0) { c->cooloff--; return; }

    int have = c->n < CAD_HIST ? c->n : CAD_HIST;
    uint64_t lo = c->cost[0];
    for (int i = 1; i < have; i++) if (c->cost[i] < lo) lo = c->cost[i];

    int budget = (int)lo + 1;              /* +1: em_now_ms has 1 ms teeth */
    if (budget < 1) budget = 1;

    /* MORE THAN HALF THE PERIOD IS NOT A CADENCE, it is a hog with a timer.
     * An app whose frame costs more than half its pace cannot be run at that
     * pace on this machine whatever the scheduler does, and asking for it
     * would claim more than half a core continuously -- which admission
     * control rightly refuses, and which would deny the claim to an app that
     * could actually keep it. Say so once and stop asking. */
    if (budget * 2 > pace) {
        if (c->period) { embk_sched_period(0, 0); c->period = 0; c->budget = 0; }
        if (!c->said) {
            char b[144];
            snprintf(b, sizeof b,
                     "%s: a frame costs %d ms of CPU, more than half the %d ms "
                     "pace -- not asking for a cadence this machine cannot "
                     "keep\n", title, budget, pace);
            embk_puts(1, b);
            c->said = 1;
        }
        c->cooloff = CAD_COOLOFF;
        return;
    }

    if (c->period == pace && c->budget == budget) return;   /* nothing new */

    int rc = embk_sched_period((uint32_t)pace, (uint32_t)budget);
    if (rc == 0) {
        c->period = pace; c->budget = budget;
        if (!c->said) {
            char b[96];
            snprintf(b, sizeof b, "%s: pacing %d ms, budget %d ms\n",
                     title, pace, budget);
            embk_puts(1, b);
            c->said = 1;
        }
    } else {
        /* Refused -- the machine has promised out as much as it will. Not an
         * error: run the ordinary way, exactly as every app did before this
         * existed. */
        cad_say(c, title, "no cadence available (the machine is fully "
                          "reserved) -- running unpaced");
        c->period = 0; c->budget = 0;
        c->cooloff = CAD_COOLOFF;
    }
}

static void cad_idle_tick(struct cadence *c) {
    if (c->heat > 0) c->heat--;
    if (c->period && c->heat == 0) {
        embk_sched_period(0, 0);
        c->period = 0; c->budget = 0;
    }
}

/* Thin adapters: the kit's clipboard signatures are its own (it must not know
 * about embk), and embk_clip_get returns what the clipboard HOLDS, which can
 * exceed the buffer -- so clamp before reporting how much was written. */
static int emapp_clip_set(const char *buf, unsigned len) {
    return embk_clip_set(buf, len);
}
static int emapp_clip_get(char *buf, unsigned cap) {
    int64_t held = embk_clip_get(buf, cap);
    if (held < 0) return 0;
    return held > (int64_t)cap ? (int)cap : (int)held;
}

int em_app_run(const EmApp *app) {
    g_app_exit_requested = 0;
    g_app_exit_code = 0;
    if (!app || !app->view) return 1;
    const char *title = app->title ? app->title : "EmApp";
    uint64_t t0 = embk_uptime_ms();

    /* resources + toolkit */
    em_res_set_loader(emapp_load);
    uint32_t fr = em_font(app->font ? app->font : "/system/fonts/font.ttf");
    { char b[96]; snprintf(b, sizeof b, "%s: font loaded (+%lums)\n", title,
                           (unsigned long)(embk_uptime_ms() - t0)); embk_puts(1, b); }

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    em_theme_use(app->theme);
    /* Then let the USER's preferences win over the app's declared default.
     * Doing it here, once, is what makes a settings app real: an application
     * does not opt in to being themed, it simply is -- the alternative is
     * every app remembering to read a config file, which means most of them
     * won't and the desktop is inconsistent by default. */
    int dock_band;
    { struct oscfg cfg; oscfg_load(&cfg);
      ui_theme_use_dark(cfg.dark != 0);
      ui_theme_set_scale((float)cfg.ui_scale / 100.0f);
      const struct oscfg_accent *a = &oscfg_accents[cfg.accent];
      ui_theme_set_accent((struct color){ a->r, a->g, a->b, 1.0f });
      /* how much of the bottom of the screen belongs to the dock */
      dock_band = oscfg_dock_band(&cfg); }
    ui_init(&sa, &la);
    em_set_clock(embk_uptime_ms);

    /* the window: requested content size, clamped + centred on the screen */
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    int winw = app->fullscreen && sw ? (int)sw : (app->size.w > 0 ? app->size.w : 640);
    int winh = app->fullscreen && sh ? (int)sh : (app->size.h > 0 ? app->size.h : 480);
    int glass = (app->material == Acrylic);      /* frosted window (implies chromeless) */
    int translucent = (app->material == Translucent);  /* per-pixel transparent, no blur */
    int chromeless = app->fullscreen || (app->chrome == Chromeless) || glass || translucent;
    int bar = chromeless ? 0 : 26;
    if (!app->fullscreen && sw && winw > (int)sw - 8)        winw = (int)sw - 8;
    /* 64 was hard-coded here and it was WRONG at every dock size: the pill is
     * dock_size+32 tall before the gap, so 84px at the default and 106 at the
     * largest. Every app window therefore covered the top of the dock -- and
     * because the dock is drawn by the desktop, which sits behind all windows,
     * and pointer input goes to the topmost window under the cursor, those
     * clicks went to the app instead. From the outside: the dock needs several
     * clicks to launch anything, and once one app is open it stops working
     * altogether. Ask the preference instead of guessing. */
    if (!app->fullscreen && sh && winh > (int)sh - 32 - dock_band - bar)
        winh = (int)sh - 32 - dock_band - bar;
    if (winw < 200) winw = 200;
    /* chromeless strips (menu bars, docks) are legitimately short; normal
     * titled windows keep a usable floor. */
    if (winh < (chromeless ? 24 : 160)) winh = chromeless ? 24 : 160;
    int wx = app->fullscreen ? 0 : ((int)sw - winw) / 2;        if (wx < 0) wx = 0;
    int wy = app->fullscreen ? 0 : 32 + ((int)sh - 32 - dock_band - winh - bar) / 2;
    if (wy < 32 && !app->fullscreen) wy = 32;
    /* Publish AFTER both are assigned. This line used to pass g_viewport_h to
     * em_set_viewport one statement BEFORE that variable was given a value, so
     * every application started life telling the toolkit its window was zero
     * pixels tall -- and nothing corrected it until the first resize.
     *
     * Anything sized from the viewport height was therefore wrong until you
     * dragged the window: the Terminal computed a NEGATIVE row count and
     * clamped to its floor of three (one visible transcript line), and a
     * ScrollView asked for viewport-minus-chrome got a negative height and
     * rendered nothing at all. "Make the window smaller and the content
     * appears" was the bug reporting itself -- resizing was simply the only
     * code path that ever published a real height. */
    g_viewport_w = (float)winw;
    g_viewport_h = (float)winh;
    em_set_viewport(g_viewport_w, g_viewport_h);

    uint32_t *px = 0;
    uint64_t wflags = translucent ? EMBK_WINF_TRANSLUCENT
                    : glass       ? EMBK_WINF_GLASS
                    : chromeless  ? EMBK_WINF_CHROMELESS : 0;
    int win = embk_win_create_shared_ex((uint32_t)winw, (uint32_t)winh, wx, wy, title,
                                        wflags, (void **)&px);
    if (win < 0 || !px) {
        char b[64]; snprintf(b, sizeof b, "%s: win_create FAILED\n", title); embk_puts(1, b);
        return 1;
    }
    if (chromeless) {                    /* the view's WindowBar drags us */
        em_window_set_mover(emapp_mover);
        em_window_bind(win, wx, wy);
    }
    em_window_set_resizable(app->resize == Resizable);
    em_window_set_glass(glass);          /* Window() adds a faint accent cast when glass */
    embk_key_grab(1);

    /* HAND THE WIDGET KIT A CLIPBOARD. It cannot call embk_clip_set itself --
     * it is built and unit-tested on the host, where there is no kernel to ask
     * -- so the runtime installs the real one and the kit calls through. This
     * is what makes copy and cut work in every field in every application
     * without a single line in any of them. */
    ui_clipboard_provider(emapp_clip_set, emapp_clip_get);

    /* THE BACK BUFFER, and the reason for it.
     *
     * `px` is SHARED with the compositor: it composites from that memory on its
     * own schedule, not ours. Rendering straight into it means every frame is
     * visible while it is still being drawn. That is harmless when a view
     * builds in a millisecond and ruinous the moment an app competes for the
     * core with a busy worker -- a browser fetching over TLS, say -- because
     * then a half-drawn frame is on screen for a long time. Worse, a full
     * repaint begins by CLEARING those pixels, so what the user sees is an
     * empty window until the redraw catches up.
     *
     * So we draw into memory nobody else can see and copy the FINISHED frame
     * across. The copy is a fraction of the render it protects, and only the
     * region being presented is copied. Falling back to direct rendering when
     * the allocation fails keeps a low-memory machine working, just tearing. */
    uint32_t *back = (uint32_t *)malloc((size_t)winw * (size_t)winh * 4);
    struct render_target rt = { back ? back : px, (uint32_t)winw, (uint32_t)winh,
                                (uint32_t)winw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    /* A translucent window may paint no background at all, so nothing would
     * erase what it drew last frame (see render_target.clear_dirty). */
    rt.clear_dirty = translucent;
    if (back) memcpy(back, px, (size_t)winw * (size_t)winh * 4);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    { char b[64]; snprintf(b, sizeof b, "%s: interactive loop running\n", title); embk_puts(1, b); }

    int pace = app->pace_ms > 0 ? app->pace_ms : 10;
    int prev_epoch = em_ui_epoch(), first = 1;
    int maximized = 0;
    /* Where the window is RIGHT NOW, and what it was before a placement moved
     * it. The runtime never knew its own position before: it told the
     * compositor where to put the window and then forgot, which is fine while
     * "restore" is the only thing that ever moves it back. With six
     * placements, going Left then Right has to restore the size the window had
     * when it was last normal, so both have to be tracked. */
    EmWinLayout layout_now = EmWinNormal;
    int wx_now = wx, wy_now = wy;
    int normal_x = wx, normal_y = wy, normal_w = winw, normal_h = winh;
    int thin_h = winh, menu_expanded = 0;   /* translucent menu-bar auto-grow */
    struct embk_win_input prev_in; memset(&prev_in, 0, sizeof prev_in);
    uint64_t last_app_tick = 0;
    struct cadence cad; memset(&cad, 0, sizeof cad);

    for (;;) {
        /* THE WHOLE ACTIVE PART OF AN ITERATION, IN CPU RATHER THAN WALL TIME.
         *
         * Two things had to be right here and neither was obvious. The span
         * has to cover the input polling, the idle hook and the present's own
         * copy, not just build-render-present -- timing only the render gave a
         * budget the app exhausted inside its own frame and then spent the
         * rest of every period unprivileged.
         *
         * And it has to be CPU, not the clock. On a contended machine wall
         * time is mostly preemption: measured with em_now_ms(), a frame whose
         * real work was a few milliseconds read as 40 ms, and the app then
         * refused to ask for a cadence it could comfortably have kept.
         * embk_thread_cpu_ns() is what the kernel actually charged us. */
        uint64_t iter_c0 = embk_thread_cpu_ns();

        /* --- inputs (always polled; they are the retained-update triggers) --- */
        if (g_em_idle_hook) g_em_idle_hook();   /* every iteration, even idle --
                                                 * the terminal's pipe poll */
        struct embk_win_input in;
        embk_win_input(&in);

        /* WHERE THE WINDOW GOES, from the zoom light's board (the toolkit
         * posts a layout during the view) or from the compositor's own
         * title-bar action. Both arrive here and take the SAME path, so the
         * button and the chrome cannot end up doing two different things.
         *
         * The resize has to happen in the client: it owns the shared pixel
         * mapping, and the toolkit's scene renderer has to be rebuilt around
         * the new one. */
        EmWinLayout want = EmWinNormal;
        int have_want = em_window_take_layout(&want);
        if (!have_want && em_window_take_maximize()) { want = EmWinFill; have_want = 1; }
        if (!have_want && in.win == EMBK_WIN_ACTION_MAXIMIZE) {
            /* The kernel chrome's zoom button is a TOGGLE, because it has no
             * board to offer: fill, or go back. */
            want = layout_now == EmWinFill ? EmWinNormal : EmWinFill;
            have_want = 1;
        }

        if (have_want && !app->fullscreen) {
            /* THE WORK AREA: the display minus the top bar and minus the dock
             * band. Every placement but Full is measured inside it, which is
             * what makes "Fill" different from "Full" and why both are offered
             * -- one leaves the shell reachable, the other does not. */
            int ax = 0, ay = 32;
            int aw = (int)sw, ah = (int)sh - 32 - dock_band;
            int nx = normal_x, ny = normal_y, nw = normal_w, nh = normal_h;

            switch (want) {
            case EmWinNormal:                                            break;
            case EmWinFill:   nx = ax; ny = ay; nw = aw; nh = ah - 26;    break;
            case EmWinFull:   nx = 0;  ny = 0;  nw = (int)sw; nh = (int)sh; break;
            case EmWinLeft:   nx = ax; ny = ay; nw = aw / 2; nh = ah - 26; break;
            case EmWinRight:  nx = ax + aw / 2; ny = ay; nw = aw - aw / 2; nh = ah - 26; break;
            case EmWinTop:    nx = ax; ny = ay; nw = aw; nh = (ah - 26) / 2; break;
            case EmWinBottom: nh = (ah - 26) / 2; nx = ax; ny = ay + nh;  nw = aw; break;
            }
            if (nw < 200) nw = 200;
            if (nh < 160) nh = 160;

            /* Remember where it was, but only when LEAVING the normal state --
             * otherwise going Left and then Right would record the left half
             * as "normal" and there would be no way back to the size the
             * window actually had. */
            if (layout_now == EmWinNormal && want != EmWinNormal) {
                normal_x = wx_now; normal_y = wy_now; normal_w = winw; normal_h = winh;
            }

            uint32_t *npx = 0;
            if (embk_win_resize(win, (uint32_t)nw, (uint32_t)nh, (void **)&npx) >= 0 && npx) {
                px = npx; winw = nw; winh = nh;
                g_viewport_w = (float)winw; g_viewport_h = (float)winh; em_set_viewport((float)winw, (float)winh);
                if (back) back = (uint32_t *)realloc(back, (size_t)winw * (size_t)winh * 4);
                rt.pixels = back ? back : px;
                rt.width = (uint32_t)winw; rt.height = (uint32_t)winh;
                rt.stride = (uint32_t)winw * 4;
                embk_win_move(win, nx, ny);
                wx_now = nx; wy_now = ny;
                scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
                em_request_frame();
                layout_now = want;
                maximized = (want == EmWinFill || want == EmWinFull);
            }
        }

        /* Translucent menu bar: a dropdown can't paint outside its window, so
         * grow the (thin) bar window tall enough to show the open dropdown, then
         * shrink back on close. The extra height is fully transparent, so the
         * bar still reads as a thin strip; only the dropdown paints into it. */
        if (translucent) {
            int want = em_menu_any_open();
            if (want != menu_expanded) {
                int32_t wyc = 0; em_window_pos(0, &wyc);
                int nh = want ? (int)sh - wyc - 8 : thin_h;
                if (want && nh > 340) nh = 340;
                if (nh < thin_h) nh = thin_h;
                uint32_t *npx = 0;
                if (embk_win_resize(win, (uint32_t)winw, (uint32_t)nh, (void **)&npx) >= 0 && npx) {
                    px = npx; winh = nh;
                    if (back) back = (uint32_t *)realloc(back, (size_t)winw * (size_t)winh * 4);
                    g_viewport_h = (float)winh;
                    em_set_viewport((float)winw, (float)winh);   /* the menu scrim
                        sizes itself to THIS -- unmirrored, it stayed bar-thin
                        and the dismiss click sailed under it */
                    rt.pixels = back ? back : px;
                    rt.height = (uint32_t)winh; rt.stride = (uint32_t)winw * 4;
                    scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
                    em_request_frame();
                    menu_expanded = want;
                }
            }
        }
        g_app_win = win;
        if (g_blur_dirty) {
            embk_win_blur_rect(win, g_blur_rect[0], g_blur_rect[1],
                               g_blur_rect[2], g_blur_rect[3]);
            g_blur_dirty = 0;
        }
        int had_key = 0;
        for (int c; (c = embk_key_poll()) != 0; ) {
            /* THE HOOK GETS FIRST REFUSAL ON PASTE TOO. The replay below turns
             * newlines and tabs into spaces, which is right for a terminal --
             * a paste must never execute anything -- and wrong for an editor,
             * where it silently flattens a pasted block onto one line. An app
             * that wants the raw clipboard handles 0x16 itself; every app that
             * does not still gets the safe replay. */
            /* TWO KEYS, ONE PASTE. Ctrl+V is what this OS has always used and
             * what its terminal-shaped apps expect; EMBK_KEY_PASTE is the GUI
             * key's (Cmd+V), which is where the rest of the editing commands
             * live now -- Ctrl+C could never be copy here, because 0x03 is the
             * console interrupt. Keeping both means nothing that worked stops
             * working. */
            if ((c == 0x16 || c == EMBK_KEY_PASTE) &&
                g_em_key_hook && g_em_key_hook(c)) { had_key = 1; continue; }
            if (c == 0x16 || c == EMBK_KEY_PASTE) {   /* paste, app-wide */
                /* The runtime replays the clipboard through the SAME delivery
                 * path as typing -- key hook first, else the focused field --
                 * so every app that can take a keystroke can take a paste,
                 * with zero code of its own. Control bytes become spaces: a
                 * paste must never EXECUTE anything (a newline reaching a
                 * terminal's Enter path would run half a paste as commands). */
                static char clip[4096];
                int64_t held = embk_clip_get(clip, sizeof clip);
                int n = held < 0 ? 0 : (held > (int64_t)sizeof clip ? (int)sizeof clip : (int)held);
                for (int i = 0; i < n; i++) {
                    int ch = (unsigned char)clip[i];
                    if (ch == '\n' || ch == '\t') ch = ' ';
                    /* CONTROL BYTES OUT, UTF-8 THROUGH. The old bound was
                     * 0x20..0x7E, which dropped every byte of every accented
                     * character: you could TYPE é into a field (the driver
                     * encodes UTF-8 now) and then not paste it, in the same
                     * field, on the same machine. 0x80 and above is a byte of a
                     * real character; the bytes of one arrive consecutively and
                     * the caret advances one at a time, so it lands intact. */
                    if (ch < 0x20 || ch == 0x7F) continue;
                    if (g_em_key_hook && g_em_key_hook(ch)) continue;
                    ui_input_char(ch);
                }
                had_key = 1;
                continue;
            }
            if (g_em_key_hook && g_em_key_hook(c)) { had_key = 1; continue; }
            if (c == 27) {
                embk_win_destroy(win); embk_key_grab(0); free(back);
                char b[64]; snprintf(b, sizeof b, "%s: exit\n", title); embk_puts(1, b);
                return 0;
            }
            ui_input_char(c);
            had_key = 1;
        }
        int input_edge = had_key ||
                         in.focused != prev_in.focused ||
                         (in.focused && (in.x != prev_in.x || in.y != prev_in.y ||
                                         in.buttons != prev_in.buttons || in.wheel));
        prev_in = in;

        /* --- retained updates: skip ALL ui work on untouched frames --------- *
         * em_overlay_active() MUST be a build trigger: a modal's scrim-dismiss
         * is debounced for a few consecutive shown frames (em.c's g_ov_frames
         * >= 3, to swallow the click that opened it). Without this, a retained
         * loop only builds on input, so the debounce counter never advances and
         * the scrim-dismiss never fires (it'd take repeated clicks). Keeping
         * frames coming while a modal is up costs nothing -- modals are
         * transient -- and matches the always-build loop the modal was designed
         * against. */
        int win_moved = em_window_moved();   /* read-and-clear (drag/snap last frame) */
        /* refresh_ms was honoured for WIDGETS only, so an application whose
         * view shows something the world changes on its own -- a clock, or the
         * menu bar sampling the wallpaper under it -- only updated when the
         * user happened to move the mouse. Same field, same meaning, both
         * paths. 0 keeps the pure input-driven behaviour. */
        /* An app that is BUSY wants to be ticked even though its refresh_ms is
         * 0 the rest of the time -- a browser polling a fetch, say. It cannot
         * do that with em_request_frame(), because that is set FROM THE VIEW
         * and the view only runs when a frame was already requested: the first
         * frame that does not ask ends the loop for good. A periodic tick is
         * computed out here, independent of whether the view ran. */
        int rms = g_refresh_override >= 0 ? g_refresh_override : app->refresh_ms;
        int app_tick = rms > 0 && (em_now_ms() - last_app_tick) >= (uint64_t)rms;
        if (app_tick) last_app_tick = em_now_ms();
        int build = first || input_edge || em_take_frame_request() || app_tick ||
                    em_ui_epoch() != prev_epoch || em_nav_transitioning() ||
                    em_overlay_active() || win_moved;
        if (!build) {
            cad_idle_tick(&cad);
            /* SAMPLE THE POINTER WHILE IDLING, in slices, rather than sleeping
             * through it. A frame here can be far apart from the next under an
             * emulator, and a press and release that both happen in the gap
             * were simply never seen: the click did not arrive late, it did not
             * arrive. Feeding the toolkit during the wait turns those into real
             * press edges, which it counts. */
            for (int slept = 0; slept < pace; slept += 5) {
                embk_sleep_ms(pace - slept < 5 ? pace - slept : 5);
                struct embk_win_input pin;
                embk_win_input(&pin);
                em_feed_pointer((float)pin.x, (float)pin.y,
                                pin.buttons & EMBK_MOUSE_LEFT,
                                pin.buttons & EMBK_MOUSE_RIGHT,
                                pin.wheel, pin.focused);
                if ((pin.buttons & EMBK_MOUSE_LEFT) != (in.buttons & EMBK_MOUSE_LEFT))
                    { em_request_frame(); break; }
            }
            continue;
        }

        em_feed_pointer((float)in.x, (float)in.y,
                        in.buttons & EMBK_MOUSE_LEFT, in.buttons & EMBK_MOUSE_RIGHT,
                        in.wheel, in.focused);

        /* full clear+repaint on structural frames (epoch bump / first), and while
         * a close pull animates (the fade/slide moves the whole window, which the
         * dirty-rect present would otherwise ghost). */
        bool force_full = false;
        if (first || em_ui_epoch() != prev_epoch || em_window_pulling() || win_moved) {
            const struct ui_theme *t = ui_theme();
            /* translucent windows clear to fully TRANSPARENT so their empty
             * canvas reveals the desktop; opaque/glass windows clear to bg. */
            uint32_t bg = translucent ? 0x00000000u
                        : (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                        | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
            uint32_t *clr = back ? back : px;
            for (int i = 0; i < winw * winh; i++) clr[i] = bg;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_epoch = em_ui_epoch();
            force_full = true;
        }

        ui_frame_begin(); em_new_frame(); app->view(); em_flush(); ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        if (g_em_post_layout) g_em_post_layout();
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        /* Park, don't quit: the app keeps running with no pixels on screen
         * until its dock icon calls win_restore. Checked before the close
         * path so a frame can't do both. */
        if (em_window_minimized()) embk_win_minimize(win);
        if (em_window_maximized()) em_window_post_maximize();   /* acted on next pass */

        if (g_app_exit_requested || em_window_closed() || em_window_take_close()) {
            /* Give the reservation back before leaving. The thread dying would
             * do it too, but em_app_run RETURNS -- an app that goes on to do
             * something else must not still be holding a claim on a cadence it
             * no longer has. */
            if (cad.period) embk_sched_period(0, 0);
            embk_win_destroy(win); embk_key_grab(0); free(back);
            char b[64]; snprintf(b, sizeof b, "%s: window closed cleanly\n", title); embk_puts(1, b);
            return g_app_exit_requested ? g_app_exit_code : 0;
        }

        if (force_full || r.full || r.n_dirty == 0) {
            if (back) memcpy(px, back, (size_t)winw * (size_t)winh * 4);
            embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
        } else {
            int x0 = 1 << 29, y0 = 1 << 29, x1 = -(1 << 29), y1 = -(1 << 29);
            /* a scroll blit moved pixels the dirty rects do not cover: the
             * whole blitted region must reach the compositor */
            if (r.has_scroll_present) {
                x0 = (int)r.sp_x; y0 = (int)r.sp_y;
                x1 = (int)(r.sp_x + r.sp_w) + 1;
                y1 = (int)(r.sp_y + r.sp_h) + 1;
            }
            for (int i = 0; i < r.n_dirty; i++) {
                int a = (int)r.dirty[i].x, b = (int)r.dirty[i].y;
                int c = (int)(r.dirty[i].x + r.dirty[i].w) + 1, d = (int)(r.dirty[i].y + r.dirty[i].h) + 1;
                if (a < x0) x0 = a;
                if (b < y0) y0 = b;
                if (c > x1) x1 = c;
                if (d > y1) y1 = d;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > winw) x1 = winw;
            if (y1 > winh) y1 = winh;
            if (x1 > x0 && y1 > y0) {
                /* only the band that changed crosses over */
                if (back)
                    for (int y = y0; y < y1; y++)
                        memcpy(px + (size_t)y * winw + x0, back + (size_t)y * winw + x0,
                               (size_t)(x1 - x0) * 4);
                embk_win_present_rect(win, px, (uint32_t)winw, (uint32_t)winh, x0, y0, x1 - x0, y1 - y0);
            }
        }

        /* The frame is on screen. What it COST is the budget estimate -- see
         * the cadence note above for why the minimum of several is used and
         * not the last one. */
        cad_frame(&cad, pace, (embk_thread_cpu_ns() - iter_c0) / 1000000u, title);

        if (first) {
            first = 0;
            char b[96]; snprintf(b, sizeof b, "%s: first frame presented (+%lums)\n", title,
                                 (unsigned long)(embk_uptime_ms() - t0)); embk_puts(1, b);
        }

        /* grip released with a real delta -> re-back the window at the new
         * size; the old pixel pointer dies inside embk_win_resize. */
        int dw = 0, dh = 0;
        if (em_window_take_resize(&dw, &dh)) {
            int nw = winw + dw, nh = winh + dh;
            if (nw < 200) nw = 200;
            if (nh < 160) nh = 160;
            if (sw && nw > (int)sw) nw = (int)sw;
            if (sh && nh > (int)sh) nh = (int)sh;
            uint32_t *npx = 0;
            if (embk_win_resize(win, (uint32_t)nw, (uint32_t)nh, (void **)&npx) >= 0 && npx) {
                px = npx; winw = nw; winh = nh;
                g_viewport_w = (float)winw; g_viewport_h = (float)winh; em_set_viewport((float)winw, (float)winh);
                if (back) {
                    uint32_t *nb = (uint32_t *)realloc(back, (size_t)winw * (size_t)winh * 4);
                    back = nb;                       /* NULL -> direct rendering, still correct */
                }
                rt.pixels = back ? back : px;
                rt.width = (uint32_t)winw; rt.height = (uint32_t)winh;
                rt.stride = (uint32_t)winw * 4;
                const struct ui_theme *t = ui_theme();
                uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                            | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
                uint32_t *clr = back ? back : px;
                for (int i = 0; i < winw * winh; i++) clr[i] = bg;
                scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
                ui_frame_begin(); em_new_frame(); app->view(); em_flush(); ui_frame_end();
                ui_run_layout((float)winw, (float)winh);
                if (g_em_post_layout) g_em_post_layout();
                scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
                if (back) memcpy(px, back, (size_t)winw * (size_t)winh * 4);
                embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
                char b[64]; snprintf(b, sizeof b, "%s: resized to %dx%d\n", title, winw, winh); embk_puts(1, b);
            }
        }
        embk_sleep_ms(pace);
    }
}

/* ======================================================================= */
/* em_widget_run -- the DESKTOP WIDGET runtime (V5).                       */
/* A trimmed em_app_run: EMBK_WINF_WIDGET window at a fixed position, no    */
/* keyboard grab, no close control, and an optional refresh_ms timer that   */
/* re-runs the view (a clock ticks with refresh_ms=1000; everything else    */
/* stays fully retained/idle).                                             */
/* ======================================================================= */

int em_widget_run(const EmWidget *wg) {
    if (!wg || !wg->view) return 1;
    const char *title = wg->title ? wg->title : "Widget";

    em_res_set_loader(emapp_load);
    uint32_t fr = em_font(wg->font ? wg->font : "/system/fonts/font.ttf");

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    em_theme_use(wg->theme);
    ui_init(&sa, &la);
    em_set_clock(embk_uptime_ms);

    int winw = wg->size.w > 0 ? wg->size.w : 190;
    int winh = wg->size.h > 0 ? wg->size.h : 96;
    uint32_t *px = 0;
    int glass = (wg->material == Acrylic);
    uint64_t wflags = EMBK_WINF_WIDGET | (glass ? EMBK_WINF_GLASS : 0);
    int win = embk_win_create_shared_ex((uint32_t)winw, (uint32_t)winh,
                                        wg->pos.x, wg->pos.y, title,
                                        wflags, (void **)&px);
    if (win < 0 || !px) {
        char b[64]; snprintf(b, sizeof b, "%s: widget create FAILED\n", title); embk_puts(1, b);
        return 1;
    }

    /* Same back buffer as em_app_run, and for the same reason: `px` is shared
     * with the compositor, which composites from it whenever it likes, so
     * drawing into it directly puts half-finished frames on screen. A widget is
     * small enough that this has never been visible -- but "too fast to catch"
     * is a property of today's widgets, not a property of the design, and a
     * glass widget already clears its whole window on every single build. The
     * buffer is winw*winh*4, which for a 190x96 clock is 73KB.
     *
     * A failed allocation renders straight into `px`, which is exactly what
     * every widget did before this. */
    uint32_t *back = (uint32_t *)malloc((size_t)winw * (size_t)winh * 4);
    if (back) memcpy(back, px, (size_t)winw * (size_t)winh * 4);
    struct render_target rt = { back ? back : px, (uint32_t)winw, (uint32_t)winh,
                                (uint32_t)winw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    { char b[64]; snprintf(b, sizeof b, "%s: widget up\n", title); embk_puts(1, b); }

    int pace = wg->pace_ms > 0 ? wg->pace_ms : 50;   /* widgets idle harder */
    struct cadence cad; memset(&cad, 0, sizeof cad);
    int prev_epoch = em_ui_epoch(), first = 1;
    uint64_t last_tick = 0;
    struct embk_win_input prev_in; memset(&prev_in, 0, sizeof prev_in);

    for (;;) {
        struct embk_win_input in;
        embk_win_input(&in);
        int input_edge = in.focused != prev_in.focused ||
                         (in.focused && (in.x != prev_in.x || in.y != prev_in.y ||
                                         in.buttons != prev_in.buttons));
        prev_in = in;

        uint64_t now = embk_uptime_ms();
        int tick = wg->refresh_ms > 0 && (now - last_tick) >= (uint64_t)wg->refresh_ms;

        int build = first || input_edge || tick || em_take_frame_request() ||
                    em_ui_epoch() != prev_epoch;
        if (!build) { cad_idle_tick(&cad); embk_sleep_ms(pace); continue; }
        if (tick) last_tick = now;

        if (in.focused) ui_pointer((float)in.x, (float)in.y, (in.buttons & EMBK_MOUSE_LEFT) != 0);
        else            ui_pointer(-100.0f, -100.0f, false);

        if (glass) {
            /* a glass widget renders a translucent tint the compositor lays over
             * the blurred aurora -- clear to TRANSPARENT + full-render every build
             * (dirty-rect would re-blend the translucent bg and accumulate). */
            uint32_t *clr = back ? back : px;
            for (int i = 0; i < winw * winh; i++) clr[i] = 0;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_epoch = em_ui_epoch();
        } else if (first || em_ui_epoch() != prev_epoch) {
            const struct ui_theme *t = ui_theme();
            uint32_t bg = (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                        | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
            uint32_t *clr = back ? back : px;
            for (int i = 0; i < winw * winh; i++) clr[i] = bg;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_epoch = em_ui_epoch();
        }

        uint64_t frame_c0 = embk_thread_cpu_ns();
        ui_frame_begin(); em_new_frame(); wg->view(); em_flush(); ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
        if (back) memcpy(px, back, (size_t)winw * (size_t)winh * 4);
        embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
        cad_frame(&cad, pace, (embk_thread_cpu_ns() - frame_c0) / 1000000u, title);

        if (first) { first = 0; char b[64]; snprintf(b, sizeof b, "%s: widget first frame\n", title); embk_puts(1, b); }
        /* The same slicing as the idle path above, and for the same reason:
         * this sleep sits between two rendered frames, which is exactly where a
         * fast second or third click lands. Sleeping through it loses the press
         * entirely -- a double click arrives as one. */
        for (int slept = 0; slept < pace; slept += 5) {
            embk_sleep_ms(pace - slept < 5 ? pace - slept : 5);
            struct embk_win_input pin;
            embk_win_input(&pin);
            em_feed_pointer((float)pin.x, (float)pin.y,
                            pin.buttons & EMBK_MOUSE_LEFT,
                            pin.buttons & EMBK_MOUSE_RIGHT,
                            pin.wheel, pin.focused);
            if ((pin.buttons & EMBK_MOUSE_LEFT) != (in.buttons & EMBK_MOUSE_LEFT))
                { em_request_frame(); break; }
        }
    }
}
