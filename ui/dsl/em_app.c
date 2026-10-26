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
#include "appearance.h"   /* the user's preferences apply to every app */
#include "ui.h"
#include "kit.h"   /* ui_clipboard_provider: the kit needs one handed to it */
#include "em.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"

static int g_app_exit_requested;
static int g_app_exit_code;
static int (*g_em_close_hook)(void);
static int g_key_modifiers,g_key_code,g_key_flags,g_key_only_wanted;
static int g_close_reason;      /* EM_CLOSE_*: why the window is being closed */
int em_close_reason(void) { return g_close_reason ? g_close_reason : em_window_closed() ? EM_CLOSE_BUTTON : EM_CLOSE_OTHER; }
int em_key_modifiers(void) { return g_key_modifiers; }
int em_key_code(void) { return g_key_code; }
int em_key_flags(void) { return g_key_flags; }
static uint32_t g_pointer_buttons;
int em_pointer_buttons(void) { return (int)g_pointer_buttons; }
void em_key_hook_wants_key_only(int on) { g_key_only_wanted=on; }
void em_set_close_hook(int (*fn)(void)) { g_em_close_hook=fn; }
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
/* A window that shows through (em_window_material): wanted, and what the
 * compositor was last told. A change clears the canvas to transparent (or
 * back) and repaints all of it. */
static int   g_mat_alpha, g_mat_blur, g_mat_told = -1, g_mat_full;
void em_window_material(bool see_through, int blur) {
    if (blur < 0) blur = 0;
    if (blur > EMBK_WIN_BLUR_MAX) blur = EMBK_WIN_BLUR_MAX;
    if (!see_through) blur = 0;
    if (g_mat_alpha == (int)see_through && g_mat_blur == blur) return;
    if (g_mat_alpha != (int)see_through) g_mat_full = 1;
    g_mat_alpha = see_through; g_mat_blur = blur;
    em_window_set_clear(see_through);
    em_request_frame();
}
bool em_window_see_through(void) { return g_mat_alpha != 0; }
static float g_viewport_h;

void em_app_request_exit(int code) {
    g_app_exit_code = code;
    g_app_exit_requested = 1;
    em_request_frame();
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

/* Move the window, then believe the compositor about where it went: it keeps a
 * window clear of the bar and the dock, and a drag that went on believing its
 * own number would keep adding the part it was refused (em_window_drag_). */
static void emapp_mover(int win, int32_t x, int32_t y) {
    embk_win_move(win, x, y);
    struct embk_win_geometry g;
    if (embk_win_where(win, &g) == 0) em_window_bind(win, g.x, g.y);
}

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
static unsigned emapp_mods(void) { return (unsigned)embk_key_mods(); }
static int emapp_clip_set(const char *buf, unsigned len) {
    return embk_clip_set(buf, len);
}
static int emapp_clip_get(char *buf, unsigned cap) {
    int64_t held = embk_clip_get(buf, cap);
    if (held < 0) return 0;
    return held > (int64_t)cap ? (int)cap : (int)held;
}

/* Background snapshots keep filesystem/channel waits off the render thread.
 * Only the render thread touches theme state, fonts and retained geometry. */
static embk_mutex g_appearance_lock=EMBK_MUTEX_INIT;
static struct oscfg g_appearance_pending;
static void appearance_worker(long arg) {
    (void)arg;
    for(;;) {
        embk_sleep_ms(1000);
        struct oscfg c;oscfg_snapshot(&c);
        embk_mutex_lock(&g_appearance_lock);g_appearance_pending=c;embk_mutex_unlock(&g_appearance_lock);
    }
}

static int g_appearance_worker_started;
static void appearance_start(const struct oscfg *initial) {
    embk_mutex_lock(&g_appearance_lock);g_appearance_pending=*initial;embk_mutex_unlock(&g_appearance_lock);
    /* Long-lived services run several windows sequentially in one process. */
    if(!g_appearance_worker_started && embk_thread_create(appearance_worker,0)>=0)g_appearance_worker_started=1;
}
static int g_shell_bottom,g_shell_height=34,g_follow_display_width;

/* THE MONITORS. The desktop is one surface over every monitor
 * (embk_monitor.h); an app places, maximizes and tiles on the monitor its
 * window is on, and the top bar and dock band only shrink the primary's work
 * area. Without the layout (one monitor, or no GPU capability) the whole
 * screen is the one output. */
struct em_out { int x, y, w, h, primary; };
static struct embk_display_layout g_dl;
static int g_dl_ok;
static uint32_t g_dl_seen;
static void outputs_refresh(void) {
    g_dl.version=EMBK_DISPLAY_LAYOUT_VERSION;
    g_dl_ok=!embk_display_layout(&g_dl) && g_dl.count;
}
static struct em_out out_primary(uint32_t sw,uint32_t sh) {
    struct em_out o={0,0,(int)sw,(int)sh,1};
    if(!g_dl_ok)return o;
    for(unsigned i=0;i<g_dl.count && i<EMBK_DISPLAY_MAX_OUTPUTS;i++) {
        const struct embk_output *d=&g_dl.outputs[i];
        if((d->flags&EMBK_OUTPUT_ENABLED) && (d->flags&EMBK_OUTPUT_PRIMARY) && d->width && d->height)
            return (struct em_out){d->x,d->y,(int)d->width,(int)d->height,1};
    }
    return o;
}
static struct em_out out_at(int px,int py,uint32_t sw,uint32_t sh) {
    struct em_out best=out_primary(sw,sh);
    if(!g_dl_ok)return best;
    long long bestd=-1;
    for(unsigned i=0;i<g_dl.count && i<EMBK_DISPLAY_MAX_OUTPUTS;i++) {
        const struct embk_output *d=&g_dl.outputs[i];
        if(!(d->flags&EMBK_OUTPUT_ENABLED) || !d->width || !d->height)continue;
        long long cx=px<d->x?d->x:px>=d->x+(int)d->width?d->x+(int)d->width-1:px;
        long long cy=py<d->y?d->y:py>=d->y+(int)d->height?d->y+(int)d->height-1:py;
        long long dd=(cx-px)*(cx-px)+(cy-py)*(cy-py);
        if(bestd<0 || dd<bestd) {
            bestd=dd;
            best=(struct em_out){d->x,d->y,(int)d->width,(int)d->height,(d->flags&EMBK_OUTPUT_PRIMARY)!=0};
        }
    }
    return best;
}
void em_primary_output(int *x,int *y,int *w,int *h) {
    uint32_t sw=0,sh=0;embk_screen_size(&sw,&sh);
    if(!g_dl_ok)outputs_refresh();
    struct em_out o=out_primary(sw,sh);
    if(x)*x=o.x;
    if(y)*y=o.y;
    if(w)*w=o.w;
    if(h)*h=o.h;
}
int em_output_at(int px,int py,int *x,int *y,int *w,int *h) {
    uint32_t sw=0,sh=0;embk_screen_size(&sw,&sh);
    if(!g_dl_ok)outputs_refresh();
    struct em_out o=out_at(px,py,sw,sh);
    if(x)*x=o.x;
    if(y)*y=o.y;
    if(w)*w=o.w;
    if(h)*h=o.h;
    return o.primary;
}
void em_app_follow_display_width(int enabled){g_follow_display_width=!!enabled;}
void em_shell_band(int bottom,int height){g_shell_bottom=!!bottom;g_shell_height=height<28?28:height>44?44:height;}
static uint32_t *resize_render_buffer(uint32_t *old,int w,int h) {
    if(!old)return NULL;
    uint32_t *next=realloc(old,(size_t)w*h*4);
    if(!next)free(old); /* Direct rendering is safer than an undersized buffer. */
    return next;
}


/* Hand the compositor what this frame changed, and nothing else.
 *
 * After a full repaint, the whole window. Otherwise the bounding box of the
 * renderer's dirty rects, and of a scroll blit's region (it moved pixels the
 * rects do not name). And when NOTHING is dirty, nothing: a frame is built on
 * every hover, refresh tick and drag step, and this used to present the whole
 * window whenever such a frame changed no pixel -- a full compose of the
 * window, many times a second, for nothing (docs/DESKTOP_PERFORMANCE.md). The
 * renderer reports overflow as r->full, so an empty list means unchanged. */


/* THE DRAG ZONES, TO THE COMPOSITOR. Whenever the laid-out title bars move or
 * change, the compositor is told where they are and drags the window by them
 * itself -- in the frame the pointer moves, with no round trip through this
 * loop, and even while this app is busy. None while a modal covers the
 * window: its scrim takes those presses. A kernel without the call leaves
 * the toolkit dragging, as before. */
static void declare_drag_zones(int win) {
    static int32_t last[4][4];
    static int last_n = -1;
    int32_t now[4][4];
    int n = em_overlay_active() ? 0 : em_window_drag_rects(now, 4);
    if (n == last_n && !memcmp(now, last, (size_t)n * sizeof now[0])) return;
    /* the compositor drags only by zones it was given: with none -- a modal
     * up, or no bar laid out -- the toolkit keeps the drag it always had */
    int taken = embk_win_drag_regions(win, (const int32_t (*)[4])now, (unsigned)n);
    em_window_set_compositor_drag(n > 0 && taken == n);
    memcpy(last, now, (size_t)n * sizeof now[0]);
    last_n = n;                  /* asked once per change, whatever the answer */
}

/* THE TWO WAITS between frames (docs/DESKTOP_PERFORMANCE.md). Idle: until a UI
 * event or `ms`. In motion: until the compositor's next frame. Each falls back
 * to sleeping on a kernel without them. */
static uint32_t g_ui_seq;
void em_wait_input(int ms) {
    if (ms < 0) ms = 0;
    if (embk_ui_wait(&g_ui_seq, (uint32_t)ms) < 0) embk_sleep_ms(ms < 5 ? (uint32_t)ms : 5);
}
void em_wait_frame(int pace) {
    if (embk_win_frame(0, 100) < 0) embk_sleep_ms((uint32_t)pace);
}

static void present_changes(int win, uint32_t *px, const uint32_t *back, int winw, int winh,
                            const struct scene_renderer *r, bool full) {
    if (full || r->full) {
        if (back) memcpy(px, back, (size_t)winw * (size_t)winh * 4);
        embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
        return;
    }
    int x0 = 1 << 29, y0 = 1 << 29, x1 = -(1 << 29), y1 = -(1 << 29);
    if (r->has_scroll_present) {
        x0 = (int)r->sp_x; y0 = (int)r->sp_y;
        x1 = (int)(r->sp_x + r->sp_w) + 1;
        y1 = (int)(r->sp_y + r->sp_h) + 1;
    }
    for (int i = 0; i < r->n_dirty; i++) {
        int a = (int)r->dirty[i].x, b = (int)r->dirty[i].y;
        int c = (int)(r->dirty[i].x + r->dirty[i].w) + 1, d = (int)(r->dirty[i].y + r->dirty[i].h) + 1;
        if (a < x0) x0 = a;
        if (b < y0) y0 = b;
        if (c > x1) x1 = c;
        if (d > y1) y1 = d;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > winw) x1 = winw;
    if (y1 > winh) y1 = winh;
    if (x1 <= x0 || y1 <= y0) return;
    /* only the band that changed crosses over */
    if (back)
        for (int y = y0; y < y1; y++)
            memcpy(px + (size_t)y * winw + x0, back + (size_t)y * winw + x0, (size_t)(x1 - x0) * 4);
    embk_win_present_rect(win, px, (uint32_t)winw, (uint32_t)winh, x0, y0, x1 - x0, y1 - y0);
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
    struct oscfg appearance;oscfg_load(&appearance);
    oscfg_apply_appearance(&appearance);

    int inset_l,inset_t,inset_r,inset_b;
    oscfg_insets(&appearance,&inset_l,&inset_t,&inset_r,&inset_b);

    ui_init(&sa, &la);
    em_set_clock(embk_uptime_ms);

    /* the window: requested content size, clamped + centred on the screen */
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    outputs_refresh();g_dl_seen=g_dl.generation;
    struct em_out P=out_primary(sw,sh);   /* new windows open on the primary monitor */
    int minw=app->min_size.w>0?app->min_size.w:200;
    int minh=app->min_size.h>0?app->min_size.h:160;
    if(P.w && minw>P.w-inset_l-inset_r)minw=P.w-inset_l-inset_r;
    if(P.h && minh>P.h-inset_t-inset_b-34)minh=P.h-inset_t-inset_b-34;
    int winw = app->fullscreen && P.w ? P.w : (app->size.w > 0 ? app->size.w : 640);
    int winh = app->fullscreen && P.h ? P.h : (app->size.h > 0 ? app->size.h : 480);
    int glass = (app->material == Acrylic);      /* frosted window (implies chromeless) */
    int translucent = (app->material == Translucent);  /* per-pixel transparent, no blur */
    int chromeless = app->fullscreen || (app->chrome == Chromeless) || glass || translucent;
    int bar = chromeless ? 0 : 34;
    if (!app->fullscreen && P.w) {
        int maxw = translucent ? P.w : P.w - inset_l - inset_r - 8;
        if (winw > maxw) winw = maxw;
    }
    /* 64 was hard-coded here and it was WRONG at every dock size: the pill is
     * dock_size+32 tall before the gap, so 84px at the default and 106 at the
     * largest. Every app window therefore covered the top of the dock -- and
     * because the dock is drawn by the desktop, which sits behind all windows,
     * and pointer input goes to the topmost window under the cursor, those
     * clicks went to the app instead. From the outside: the dock needs several
     * clicks to launch anything, and once one app is open it stops working
     * altogether. Ask the preference instead of guessing. */
    if (!app->fullscreen && P.h && winh > P.h - inset_t - inset_b - bar)
        winh = P.h - inset_t - inset_b - bar;
    if(winw<minw)winw=minw;
    /* chromeless strips (menu bars, docks) are legitimately short; normal
     * titled windows keep a usable floor. */
    if(winh<(translucent?24:minh))winh=translucent?24:minh;
    int wx = app->fullscreen ? P.x : P.x+inset_l+(P.w-inset_l-inset_r-winw)/2;        if (wx < P.x) wx = P.x;
    int wy = app->fullscreen ? P.y : P.y+inset_t+(P.h-inset_t-inset_b-winh-bar)/2;
    if (wy < P.y+inset_t && !app->fullscreen) wy = P.y+inset_t;
    if (g_follow_display_width) { wx = P.x; wy = P.y; }
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
    if(strcmp(title,"TopBar"))appearance_start(&appearance);

    /* HAND THE WIDGET KIT A CLIPBOARD. It cannot call embk_clip_set itself --
     * it is built and unit-tested on the host, where there is no kernel to ask
     * -- so the runtime installs the real one and the kit calls through. This
     * is what makes copy and cut work in every field in every application
     * without a single line in any of them. */
    ui_clipboard_provider(emapp_clip_set, emapp_clip_get);
    /* ...and a clock, for the same reason: the kit cannot ask the kernel what
     * time it is, and double-click has to know how long ago the last press
     * was. Counting frames would not do -- this loop skips them when nothing
     * moves. */
    ui_clock_provider(embk_uptime_ms);
    /* ...and the modifier keys, for shift-click. A click carries no byte, so
     * unlike Shift+Left -- which the driver decides before sending -- the kit
     * has to ask what was held. */
    ui_mods_provider(emapp_mods);


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
    int drag_pending=0;
    /* Where the window is RIGHT NOW, and what it was before a placement moved
     * it. The runtime never knew its own position before: it told the
     * compositor where to put the window and then forgot, which is fine while
     * "restore" is the only thing that ever moves it back. With six
     * placements, going Left then Right has to restore the size the window had
     * when it was last normal, so both have to be tracked. */
    EmWinLayout layout_now = EmWinNormal;
    int wx_now = wx, wy_now = wy;
    int normal_x = wx, normal_y = wy, normal_w = winw, normal_h = winh;
    int geometry_pending = 0;
    int menu_keyboard = -1;
    int thin_h = winh, menu_expanded = 0;   /* translucent menu-bar auto-grow */
    struct embk_win_input prev_in; memset(&prev_in, 0, sizeof prev_in);
    uint64_t last_app_tick = 0;
    uint64_t screen_next=0;
    int display_pending=0;
    struct cadence cad; memset(&cad, 0, sizeof cad);
    int built_motion = 0;

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
        g_pointer_buttons=in.focused?in.buttons:0;

        if(em_now_ms()>=screen_next) {
            screen_next=em_now_ms()+200;uint32_t nsw=0,nsh=0;embk_screen_size(&nsw,&nsh);
            struct embk_display_layout was=g_dl;int was_ok=g_dl_ok;
            outputs_refresh();
            int layout_moved=g_dl_ok && g_dl.generation!=g_dl_seen;
            int display_changed=nsw && nsh && (nsw!=sw || nsh!=sh || layout_moved);
            if(!app->fullscreen && !g_follow_display_width) {
                /* WHERE THE PERSON PUT IT. A window moved since the runtime
                 * last placed it -- by its own title bar (chromeless), or the
                 * kernel's -- has a new normal place. The kernel's answer is
                 * only asked between display changes: right after one, the
                 * compositor's own clamping is in it. */
                int32_t x=wx_now,y=wy_now;
                if(chromeless)em_window_pos(&x,&y);
                else if(!display_changed){struct embk_win_geometry geo;if(!embk_win_where(win,&geo)){x=geo.x;y=geo.y;}}
                if(x!=wx_now || y!=wy_now) {
                    if(layout_now==EmWinNormal){normal_x=x;normal_y=y;}
                    wx_now=x;wy_now=y;
                }
                /* THE MONITOR MOVED: the compositor has carried the window
                 * with it, and the normal place goes the same distance. Only
                 * the monitor's move -- never the clamping a smaller mode
                 * forces -- so a resolution trial reverted puts every window
                 * back exactly where it was. */
                int32_t dx=0,dy=0;
                if(display_changed && was_ok && g_dl_ok &&
                   arrange_follow(&was,&g_dl,wx_now+winw/2,wy_now+winh/2,&dx,&dy) && (dx || dy)) {
                    normal_x+=dx;normal_y+=dy;wx_now+=dx;wy_now+=dy;
                    if(chromeless)em_window_bind(win,wx_now,wy_now);
                }
            }
            if(nsw && nsh && (display_pending || nsw!=sw || nsh!=sh || layout_moved)) {
                display_pending=0;g_dl_seen=g_dl.generation;
                sw=nsw;sh=nsh;
                P=out_primary(sw,sh);
                minw=app->min_size.w>0?app->min_size.w:200;
                minh=app->min_size.h>0?app->min_size.h:160;
                if(minw>P.w-inset_l-inset_r)minw=P.w-inset_l-inset_r;
                if(minh>P.h-inset_t-inset_b-bar)minh=P.h-inset_t-inset_b-bar;
                if(app->fullscreen || g_follow_display_width) {
                    int nw=P.w,nh=app->fullscreen?P.h:menu_expanded==2?P.h:winh;
                    if(nh>P.h)nh=P.h;
                    uint32_t *npx=NULL;
                    if(embk_win_resize(win,nw,nh,(void **)&npx)>=0 && npx) {
                        px=npx;winw=nw;winh=nh;back=resize_render_buffer(back,nw,nh);
                        rt.pixels=back?back:px;rt.width=nw;rt.height=nh;rt.stride=nw*4;
                        g_viewport_w=nw;g_viewport_h=nh;em_set_viewport(nw,nh);
                        int ny=P.y+(g_follow_display_width && g_shell_bottom && !menu_expanded?P.h-nh:0);
                        embk_win_place(win,P.x,ny);if(chromeless)em_window_bind(win,P.x,ny);
                        scene_render_destroy(&r);scene_render_init(&r,cpu_backend_get());em_structure_changed();em_request_frame();
                    }else display_pending=1;
                }else geometry_pending=1;
            }
        }

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
        int passive_layout=0;
        if(drag_pending && !(in.buttons&EMBK_MOUSE_LEFT)) {
            drag_pending=0;
            if(appearance.snap && app->resize==Resizable && chromeless && !have_want) {
                int32_t x,y;em_window_pos(&x,&y);wx_now=x;wy_now=y;
                struct em_out O=out_at(x+winw/2,y+winh/2,sw,sh);
                int il=O.primary?inset_l:0,ir=O.primary?inset_r:0,it=O.primary?inset_t:0;
                if(x<=O.x+il+4) {want=EmWinLeft;have_want=1;}
                else if(x+winw>=O.x+O.w-ir-4){want=EmWinRight;have_want=1;}
                else if(y<=O.y+it+4){want=EmWinFill;have_want=1;}
            }
        }
        if (!have_want && em_window_take_maximize()) { want = layout_now==EmWinFill?EmWinNormal:EmWinFill; have_want = 1; }
        /* PLEASE CLOSE -- the close light, or GUI+W. The runtime takes the app
         * down the ordinary way (the same path Escape and em_app_request_exit
         * use), so main() returns, files close and the window is destroyed by
         * its owner. Before this the compositor killed the process outright and
         * an unsaved document went with it.
         *
         * An app that wants to ask "save first?" installs a close hook.
         * Acknowledge immediately so drawing its prompt cannot race the
         * compositor's grace timeout. Apps without a hook retain that backstop. */
        if (in.win == EMBK_WIN_ACTION_CLOSE || in.win == EMBK_WIN_ACTION_QUIT) {
            g_close_reason = in.win == EMBK_WIN_ACTION_QUIT ? EM_CLOSE_QUIT : EM_CLOSE_REQUEST;
            if (g_em_close_hook) embk_win_close_ack((uint32_t)win);
            em_app_request_exit(0);
        }
        if(strcmp(title,"TopBar")) {
            struct oscfg next;
            embk_mutex_lock(&g_appearance_lock);next=g_appearance_pending;embk_mutex_unlock(&g_appearance_lock);
            if(memcmp(&next,&appearance,sizeof next)) {
                int l=inset_l,t=inset_t,r=inset_r,b=inset_b;
                appearance=next;if(strcmp(title,"Settings"))oscfg_apply_appearance(&appearance);
                oscfg_insets(&appearance,&inset_l,&inset_t,&inset_r,&inset_b);
                if((l!=inset_l || t!=inset_t || r!=inset_r || b!=inset_b) && !app->fullscreen && !translucent) {
                    geometry_pending=1;
                }
                em_structure_changed();em_request_frame();
            }
        }
        if(geometry_pending) {
            /* The desktop and app poll the file independently. Moving before
             * the desktop publishes its new work area lets the compositor
             * clamp against the OLD band (e.g. leaving a title under the bar).
             * Wait for the shared geometry contract, then reflow once. */
            struct embk_desktop area={.version=EMBK_DESKTOP_VERSION};
            int rc=embk_desktop_control(EMBK_DESKTOP_GET,&area);
            if(rc<0 || (area.left==(unsigned)inset_l && area.top==(unsigned)inset_t &&
                        area.right==(unsigned)inset_r && area.bottom==(unsigned)inset_b)) {
                if(!have_want){want=layout_now;passive_layout=1;}
                have_want=1;geometry_pending=0;
            }
        }
        /* A DROP LANDED. The payload is the application's to take
         * (embk_drop_take, in its view); all the runtime owes it is a frame in
         * which to notice -- an idle app builds none, and the drop would sit
         * unread while the drag looked like it had done nothing. */
        if (in.win == EMBK_WIN_ACTION_DROP) em_request_frame();
        /* the compositor dragged us by a declared zone and the button is up:
         * learn where we are, and let the snap below look at it */
        if (in.win == EMBK_WIN_ACTION_MOVED) {
            struct embk_win_geometry g;
            if (embk_win_where(win, &g) == 0) em_window_bind(win, g.x, g.y);
            drag_pending = 1; em_request_frame();
        }

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
            /* ...of the monitor the window is on: the bars are only on the primary. */
            struct em_out O=want==EmWinNormal?out_at(normal_x+normal_w/2,normal_y+normal_h/2,sw,sh)
                                             :out_at(wx_now+winw/2,wy_now+winh/2,sw,sh);
            int ax = O.x+(O.primary?inset_l:0), ay = O.y+(O.primary?inset_t:0);
            int aw = O.w-(O.primary?inset_l+inset_r:0), ah = O.h-(O.primary?inset_t+inset_b:0);
            int nx = normal_x, ny = normal_y, nw = normal_w, nh = normal_h;

            switch (want) {
            case EmWinNormal:
                if(nw>aw)nw=aw;
                if(nh>ah-bar)nh=ah-bar;
                if(nx<ax)nx=ax;
                if(nx>ax+aw-nw)nx=ax+aw-nw;
                if(ny<ay)ny=ay;
                if(ny>ay+ah-bar-nh)ny=ay+ah-bar-nh;
                break;
            case EmWinFill:   nx = ax; ny = ay; nw = aw; nh = ah - bar;    break;
            case EmWinFull:   nx = O.x; ny = O.y; nw = O.w; nh = O.h; break;
            case EmWinLeft:   nx = ax; ny = ay; nw = aw / 2; nh = ah - bar; break;
            case EmWinRight:  nx = ax + aw / 2; ny = ay; nw = aw - aw / 2; nh = ah - bar; break;
            case EmWinTop:    nx = ax; ny = ay; nw = aw; nh = (ah - bar) / 2; break;
            case EmWinBottom: nh = (ah - bar) / 2; nx = ax; ny = ay + nh;  nw = aw; break;
            }
            if(want!=EmWinNormal && want!=EmWinFull && (nw<minw || nh<minh)) {
                nx=ax;ny=ay;nw=aw;nh=ah-bar;want=EmWinFill;
            }
            if(nw<minw)nw=minw;
            if(nh<minh)nh=minh;

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
                if (back) back = resize_render_buffer(back,winw,winh);
                rt.pixels = back ? back : px;
                rt.width = (uint32_t)winw; rt.height = (uint32_t)winh;
                rt.stride = (uint32_t)winw * 4;
                if(passive_layout)embk_win_place(win,nx,ny);else embk_win_move(win,nx,ny);
                if(chromeless)em_window_bind(win,nx,ny);
                wx_now = nx; wy_now = ny;
                scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
                em_request_frame();
                layout_now = want;

            }
        }

        /* Translucent menu bar: a dropdown can't paint outside its window, so
         * grow the (thin) bar window tall enough to show the open dropdown, then
         * shrink back on close. The extra height is fully transparent, so the
         * bar still reads as a thin strip; only the dropdown paints into it. */
        if (translucent) {
            int want = em_menu_any_open();
            /* Chrome normally leaves keys with the app. An open dropdown
             * borrows them until dismissal, including when resize fails. */
            if (want != menu_keyboard && embk_win_menu((uint32_t)win,want) >= 0)
                menu_keyboard = want;
            int expanded=want?2:em_tooltip_visible()?1:0;
            if(expanded!=menu_expanded || (!expanded && thin_h!=g_shell_height)) {
                thin_h=g_shell_height;
                int32_t wyc = 0; em_window_pos(0, &wyc);
                /* The transparent dismiss surface reaches the dock and screen
                 * edges too; a 340px canvas silently missed clicks below it. */
                int nh=want?P.h:expanded?thin_h+48:thin_h;
                if (nh < thin_h) nh = thin_h;
                uint32_t *npx = 0;
                if (embk_win_resize(win, (uint32_t)winw, (uint32_t)nh, (void **)&npx) >= 0 && npx) {
                    px = npx; winh = nh;
                    if (back) back = resize_render_buffer(back,winw,winh);
                    g_viewport_h = (float)winh;
                    em_set_viewport((float)winw, (float)winh);   /* the menu scrim
                        sizes itself to THIS -- unmirrored, it stayed bar-thin
                        and the dismiss click sailed under it */
                    rt.pixels = back ? back : px;
                    rt.height = (uint32_t)winh; rt.stride = (uint32_t)winw * 4;
                    scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
                    em_request_frame();
                    menu_expanded = expanded;
                    em_window_move_to(P.x,P.y+(g_shell_bottom && !want?P.h-nh:0));
                }
            }
        }
        g_app_win = win;
        if (g_blur_dirty) {
            embk_win_blur_rect(win, g_blur_rect[0], g_blur_rect[1],
                               g_blur_rect[2], g_blur_rect[3]);
            g_blur_dirty = 0;
        }
        if (g_mat_told != (g_mat_alpha << 8 | g_mat_blur)) {
            if (embk_win_set_material(win, g_mat_alpha, g_mat_blur) == 0 || !g_mat_alpha)
                g_mat_told = g_mat_alpha << 8 | g_mat_blur;
            else g_mat_alpha = g_mat_blur = 0, g_mat_told = 0, g_mat_full = 1, em_window_set_clear(0);   /* refused: opaque, as it is */
        }
        int had_key = 0;
        for (int64_t input; (input = embk_key_poll_key()) > 0; ) {
            int c=(int)(input&255);g_key_modifiers=(int)((input>>8)&255);
            g_key_code=(int)((input>>16)&0xffff);g_key_flags=(int)((input>>32)&255);
            /* A key with no character (F5, Insert) reaches only a hook that
             * asked for one; to everything else it is as if never pressed. */
            if (g_key_flags&EMBK_KEYF_ONLY) {
                if (g_em_key_hook && g_key_only_wanted && !ui_focus_scope_active() && g_em_key_hook(EM_KEY_ONLY|g_key_code)) had_key=1;
                continue;
            }
            /* Escape first dismisses a menu; it must not quit the application
             * (especially the system bar) that owns an open dropdown. */
            if (em_menu_key(c)) {had_key=1;continue;}
            int captured=ui_focus_scope_active();
            /* Apps such as Terminal and Note++ own their paste policy. Other
             * apps deliver ONE command to the focused widget: replaying bytes
             * here loses multiline text and overflows the bounded key queue. */
            if (c == 0x16 || c == EMBK_KEY_PASTE) {
                if (g_em_key_hook && !captured && g_em_key_hook(c)) {
                    had_key=1; continue;
                }
                ui_input_key(UI_KEY_PASTE,(unsigned)g_key_modifiers);
                had_key=1; continue;
            }
            if (g_em_key_hook && (!captured || c==27) && g_em_key_hook(c)) { had_key = 1; continue; }
            if (c==27 && em_overlay_escape()){had_key=1;continue;}
            if (c == 27) { em_app_request_exit(0); had_key = 1; continue; }
            ui_input_key(c,(unsigned)g_key_modifiers);
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
        int win_moved = em_window_moved();
        if(win_moved && (in.buttons&EMBK_MOUSE_LEFT))drag_pending=1;   /* read-and-clear (drag/snap last frame) */
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
                    em_overlay_active() || win_moved || ui_keyboard_pending() || ui_focus_needs_frame();
        if (!build) {
            cad_idle_tick(&cad);
            /* NOTHING TO DRAW: WAIT FOR SOMETHING TO ANSWER. This used to sample
             * the pointer every 5 ms -- six still windows woke the machine twelve
             * hundred times a second to learn nothing had happened. It waits for
             * a UI event now (a key, the pointer over or clicking this window,
             * focus, a close request), or for this loop's own next job: the
             * display re-check, a refresh tick, or -- with an idle hook, the
             * terminal's pipes -- the pace. A press and release in the gap are
             * still both seen: the compositor queues presses and replays them
             * one per poll. */
            uint64_t now = em_now_ms();
            int64_t wait = screen_next > now ? (int64_t)(screen_next - now) : 0;
            if (g_em_idle_hook && wait > pace) wait = pace;
            if (rms > 0) {
                uint64_t due = last_app_tick + (uint64_t)rms;
                int64_t left = due > now ? (int64_t)(due - now) : 0;
                if (left < wait) wait = left;
            }
            em_wait_input((int)wait);
            continue;
        }

        ui_pointer_at_time(in.when);   /* when the press HAPPENED, not now */
        em_feed_pointer((float)in.x, (float)in.y,
                        in.buttons & EMBK_MOUSE_LEFT, in.buttons & EMBK_MOUSE_RIGHT,
                        in.wheel, in.focused);

        /* full clear+repaint on structural frames (epoch bump / first), and while
         * a close pull animates (the fade/slide moves the whole window, which the
         * dirty-rect present would otherwise ghost).
         *
         * NOT when the window moved. Its pixels are the same wherever it is: the
         * compositor holds them and composes them at the new place, and a blur
         * in here samples this canvas, not the desktop. A move still builds a
         * frame, for whatever follows the window; repainting all of it on every
         * step of a drag presented 647 thousand pixels a step for nothing
         * (docs/DESKTOP_PERFORMANCE.md). */
        bool force_full = false;
        if (first || em_ui_epoch() != prev_epoch || em_window_pulling() || g_mat_full) {
            const struct ui_theme *t = ui_theme();
            g_mat_full = 0;
            /* translucent windows -- and one that shows through -- clear to
             * fully TRANSPARENT so their empty canvas reveals the desktop;
             * opaque/glass windows clear to bg. */
            uint32_t bg = translucent || g_mat_alpha ? 0x00000000u
                        : (255u << 24) | ((uint32_t)(t->bg.r * 255) << 16)
                        | ((uint32_t)(t->bg.g * 255) << 8) | (uint32_t)(t->bg.b * 255);
            uint32_t *clr = back ? back : px;
            for (int i = 0; i < winw * winh; i++) clr[i] = bg;
            scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get());
            prev_epoch = em_ui_epoch();
            force_full = true;
        }

        ui_frame_begin(); em_new_frame(); app->view(); em_flush(); em_tooltip_host();ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        if (g_em_post_layout) g_em_post_layout();
        if (chromeless) declare_drag_zones(win);
        rt.clear_dirty = translucent || g_mat_alpha;     /* the material can change at any time */
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        /* Park, don't quit: the app keeps running with no pixels on screen
         * until its dock icon calls win_restore. Checked before the close
         * path so a frame can't do both. */
        if (em_window_minimized()) embk_win_minimize(win);
        if (em_window_maximized()) em_window_post_maximize();   /* acted on next pass */

        int closing = g_app_exit_requested || em_window_closed();
        /* Always consume the one-shot close, including when another source
         * already requested exit; a deferred request must not replay. */
        if (em_window_take_close()) closing = 1;
        if (closing && g_em_close_hook && !g_em_close_hook()) {
            embk_win_close_ack((uint32_t)win);
            g_app_exit_requested = 0;
            em_request_frame();
            closing = 0;
        }
        if (!closing) g_close_reason = 0;
        if (closing) {
            /* Give the reservation back before leaving. The thread dying would
             * do it too, but em_app_run RETURNS -- an app that goes on to do
             * something else must not still be holding a claim on a cadence it
             * no longer has. */
            if (cad.period) embk_sched_period(0, 0);
            embk_win_destroy(win); embk_key_grab(0); free(back);
            char b[64]; snprintf(b, sizeof b, "%s: window closed cleanly\n", title); embk_puts(1, b);
            return g_app_exit_requested ? g_app_exit_code : 0;
        }

        present_changes(win, px, back, winw, winh, &r, force_full);

        /* The frame is on screen. What it COST is the budget estimate -- see
         * the cadence note above for why the minimum of several is used and
         * not the last one. */
        cad_frame(&cad, pace, (embk_thread_cpu_ns() - iter_c0) / 1000000u, title);

        if (first) {
            first = 0;
            char b[96]; snprintf(b, sizeof b, "%s: first frame presented (+%lums)\n", title,
                                 (unsigned long)(embk_uptime_ms() - t0)); embk_puts(1, b);
        }
        built_motion = em_frame_requested() || em_nav_transitioning() || em_overlay_active() ||
                       em_window_pulling();

        /* grip released with a real delta -> re-back the window at the new
         * size; the old pixel pointer dies inside embk_win_resize. */
        int dw = 0, dh = 0;
        if (em_window_take_resize(&dw, &dh)) {
            int nw = winw + dw, nh = winh + dh;
            if(nw<minw)nw=minw;
            if(nh<minh)nh=minh;
            struct em_out O=out_at(wx_now+winw/2,wy_now+winh/2,sw,sh);   /* no bigger than its monitor */
            if (O.w && nw > O.w) nw = O.w;
            if (O.h && nh > O.h) nh = O.h;
            uint32_t *npx = 0;
            if (embk_win_resize(win, (uint32_t)nw, (uint32_t)nh, (void **)&npx) >= 0 && npx) {
                px = npx; winw = nw; winh = nh;
                if (layout_now == EmWinNormal) { normal_w = nw; normal_h = nh; }   /* the size the person chose */
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
                ui_frame_begin(); em_new_frame(); app->view(); em_flush(); em_tooltip_host();ui_frame_end();
                ui_run_layout((float)winw, (float)winh);
                if (g_em_post_layout) g_em_post_layout();
                scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
                if (back) memcpy(px, back, (size_t)winw * (size_t)winh * 4);
                embk_win_present(win, px, (uint32_t)winw, (uint32_t)winh);
                char b[64]; snprintf(b, sizeof b, "%s: resized to %dx%d\n", title, winw, winh); embk_puts(1, b);
            }
        }
        /* THE NEXT FRAME, ON THE COMPOSITOR'S BEAT. Something is moving -- a
         * frame was asked for, a page fades, a modal debounces, the window is
         * being pulled -- so wait for the compositor's next frame rather than
         * sleeping a guess at one: the motion lands on the frames that show
         * it. Nothing moving: straight back to the top, where an idle loop
         * waits for input. */
        if (built_motion) em_wait_frame(pace);
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
    struct oscfg widget_appearance;oscfg_load(&widget_appearance);oscfg_apply_appearance(&widget_appearance);
    appearance_start(&widget_appearance);

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
        g_pointer_buttons=in.focused?in.buttons:0;
        int input_edge = in.focused != prev_in.focused ||
                         (in.focused && (in.x != prev_in.x || in.y != prev_in.y ||
                                         in.buttons != prev_in.buttons));
        prev_in = in;

        uint64_t now = embk_uptime_ms();
        int tick = wg->refresh_ms > 0 && (now - last_tick) >= (uint64_t)wg->refresh_ms;

        struct oscfg next;
        embk_mutex_lock(&g_appearance_lock);next=g_appearance_pending;embk_mutex_unlock(&g_appearance_lock);
        if(memcmp(&next,&widget_appearance,sizeof next)) {
            widget_appearance=next;oscfg_apply_appearance(&next);em_structure_changed();em_request_frame();
        }
        int build = first || input_edge || tick || em_take_frame_request() ||
                    em_ui_epoch() != prev_epoch;
        if (!build) {
            /* nothing to draw: wait for input, or the next refresh tick (a
             * clock); a widget with neither waits a second at a time */
            cad_idle_tick(&cad);
            int64_t wait = 1000;
            if (wg->refresh_ms > 0) {
                uint64_t due = last_tick + (uint64_t)wg->refresh_ms;
                wait = due > now ? (int64_t)(due - now) : 0;
            }
            em_wait_input((int)wait);
            continue;
        }
        if (tick) last_tick = now;

        ui_pointer_at_time(in.when);   /* when it HAPPENED, not when we noticed */
        if (in.focused) ui_pointer((float)in.x, (float)in.y, (in.buttons & EMBK_MOUSE_LEFT) != 0);
        else            ui_pointer(-100.0f, -100.0f, false);

        /* repainted whole below: presented whole */
        bool whole = glass || first || em_ui_epoch() != prev_epoch;
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
        ui_frame_begin(); em_new_frame(); wg->view(); em_flush();em_tooltip_host();ui_frame_end();
        ui_run_layout((float)winw, (float)winh);
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);
        present_changes(win, px, back, winw, winh, &r, whole);
        cad_frame(&cad, pace, (embk_thread_cpu_ns() - frame_c0) / 1000000u, title);

        if (first) { first = 0; char b[64]; snprintf(b, sizeof b, "%s: widget first frame\n", title); embk_puts(1, b); }
        /* motion on the compositor's beat, as em_app_run; otherwise back to
         * the top, which waits for input or the next refresh */
        if (em_frame_requested()) em_wait_frame(pace);
    }
}
