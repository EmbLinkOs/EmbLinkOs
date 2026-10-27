/* user/apps/home/home.c -- the EmbLink OS HOME launcher.
 *
 * This is where the OS lands: the kernel spawns it at boot (see kernel/main.c),
 * and it takes over the whole screen as the compositor's DESKTOP layer -- a
 * full-screen, chromeless, back-pinned window (embk_win_create_desktop). App
 * windows the user launches float ON TOP of it.
 *
 * It draws a simple, nice launcher (a title + a grid of app tiles) with the
 * EmUI toolkit, rendering straight into the desktop window's shared pixel
 * pages (zero-copy). Clicks are routed to it by the compositor via
 * embk_win_input (the desktop receives every click that falls through the
 * floating windows), and clicking a tile embk_spawn()s that app. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "embk.h"
#include "appauth.h"
#include "emauth.h"   /* the screen lock asks authd, which is outside this session */
#include "appearance.h"   /* the user's dock preferences, re-read live */
#include "emnotify.h" /* banners, for the failures a log cannot show anyone */
#include "kit.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "scene_render.h"
#include "font.h"

static uint8_t *read_file(const char *path, size_t *len) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return 0;
    size_t cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (n + 65536 > cap) { cap *= 2; buf = realloc(buf, cap); }
        int64_t got = embk_read(fd, buf + n, 65536);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    *len = n;
    return buf;
}

/* set by a tile click during the UI pass; the loop acts on it after the frame */
static const char *g_launch = 0;
static const char *g_launch_dir = 0;
static char        g_clock[32] = "up 0:00";
static char        g_datetime[48] = "--:--";
static float       g_sw = 0, g_sh = 0;   /* screen size (the desktop window) */

/* THE MONITORS. The desktop window spans every one (one surface,
 * embk_monitor.h); each monitor shows a whole wallpaper of its own, and the
 * dock, the lock card and every sheet belong to the primary -- centred on the
 * whole desktop they would land across the seam between two monitors. With
 * one monitor the primary is the whole screen. */
#define HOME_MAX_OUT 4
static float    g_px, g_py, g_pw, g_ph;           /* the primary monitor */
static int      g_nout;
static float    g_ox[HOME_MAX_OUT], g_oy[HOME_MAX_OUT], g_ow[HOME_MAX_OUT], g_oh[HOME_MAX_OUT];
static uint32_t g_out_gen;
static uint32_t g_out_set;      /* which monitors are connected (display_arrange.h) */
static unsigned g_out_connected;

static void outputs_poll(void) {
    struct embk_display_layout d;
    d.version = EMBK_DISPLAY_LAYOUT_VERSION;
    g_nout = 0; g_px = g_py = 0; g_pw = g_sw; g_ph = g_sh;
    if (embk_display_layout(&d) || !d.count) return;
    g_out_gen = d.generation;
    g_out_set = arrange_connected_set(&d);
    g_out_connected = arrange_connected(&d);
    for (unsigned i = 0; i < d.count && i < EMBK_DISPLAY_MAX_OUTPUTS && g_nout < HOME_MAX_OUT; i++) {
        const struct embk_output *o = &d.outputs[i];
        if (!(o->flags & EMBK_OUTPUT_ENABLED) || !o->width || !o->height) continue;
        g_ox[g_nout] = (float)o->x; g_oy[g_nout] = (float)o->y;
        g_ow[g_nout] = (float)o->width; g_oh[g_nout] = (float)o->height;
        if (o->flags & EMBK_OUTPUT_PRIMARY) { g_px = g_ox[g_nout]; g_py = g_oy[g_nout]; g_pw = g_ow[g_nout]; g_ph = g_oh[g_nout]; }
        g_nout++;
    }
}

/* The monitor a point is on, or the nearest; 1 when it is the primary. */
static int out_of(float x, float y, float *ox, float *oy, float *ow, float *oh) {
    int best = -1; float bestd = 0;
    for (int i = 0; i < g_nout; i++) {
        float cx = x < g_ox[i] ? g_ox[i] : x > g_ox[i] + g_ow[i] - 1 ? g_ox[i] + g_ow[i] - 1 : x;
        float cy = y < g_oy[i] ? g_oy[i] : y > g_oy[i] + g_oh[i] - 1 ? g_oy[i] + g_oh[i] - 1 : y;
        float d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (best < 0 || d < bestd) { best = i; bestd = d; }
    }
    if (best < 0) { *ox = 0; *oy = 0; *ow = g_sw; *oh = g_sh; return 1; }
    *ox = g_ox[best]; *oy = g_oy[best]; *ow = g_ow[best]; *oh = g_oh[best];
    return g_ox[best] == g_px && g_oy[best] == g_py;
}

/* A zero-sized overlay root with one child at (x, y): how anything is put
 * on a particular monitor of the desktop window. */
static void at_begin(uint64_t key, float x, float y) {
    em_flush();
    ui_begin_vstack(key); ui_set_overlay(true);
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
    ui_set_clip_children(false);
    ui_begin_vstack(key ^ 0x5157ull); ui_set_offset(x, y);
}
static void at_end(void) { ui_end_stack(); ui_end_stack(); }
static char      **g_session_env = 0;

static void launch_folder(const char *path) {
    g_launch = "/data/apps/files/files.elf";
    g_launch_dir = path;
}

/* An app the user can click (launch/open) OR drag. `app` spawns an elf; `dir`
 * opens Files at a folder (one of the two is set). icon/label are BUFFERS so an
 * app can supply them from its own <name>.app presentation manifest; the stable
 * IDENTITY (for reconciler keys) is the app/dir path, never the icon buffer. */
struct app_item {
    char icon[96]; char label[24]; const char *app; const char *dir;
    /* Free placement on the desktop: every icon owns its spot rather than
     * inheriting one from its position in the list. `placed` starts 0 so a
     * newly-arrived icon is auto-arranged instead of landing at (0,0). */
    float x, y; int placed;
};

/* Desktop icon cell: the glyph plus its caption, and the gap around it.
 * DESK_ICON is the one knob for how big desktop apps look. Keep it 8 above a
 * level in the .eic ladder (docs/ICONS.md) -- the art is drawn into size-8, so
 * 48 lands exactly on the 40 level and needs no resampling. */
#define DESK_ICON   (g_cfg.desktop_icon_size)
#define DESK_CELL_W 76
#define DESK_CELL_H 76
#define DESK_MARGIN 16

/* Apps live EITHER on the desktop OR in the dock; dragging MOVES one between the
 * two (never a copy), so a name never lingers behind. Home's folder is $HOME,
 * filled in once at startup. */
static struct app_item g_desk[16] = {
    { "/system/images/setting.eic", "System", 0, "/system" },
};
static int g_desk_n = 1;

/* the bottom app dock -- a centered pill sized to its apps. Grows as apps are
 * dragged in / shrinks as they are dragged out, but never below DOCK_MIN. */
#define DOCK_MIN 2
static struct app_item g_dock[16] = {
    { "/system/images/files.eic",    "Files",    "/data/apps/files/files.elf",       0 },
    { "/system/images/terminal.eic", "Terminal", "/data/apps/term/term.elf",         0 },
    { "/system/images/setting.eic",  "Settings", "/data/apps/settings/settings.elf", 0 },
    { "/system/images/vellum.eic",   "Vellum",   "/data/apps/vellum/vellum.elf",     0 },
    { "/system/images/netsurf.eic",  "NetSurf",  "/data/apps/nsemblink/nsemblink.elf", 0 },
};
static int g_dock_n = 5;

/* Fill an item's icon+label from the app's OWN presentation manifest at
 * /data/apps/<name>/<name>.app ("name X" / "icon Y" lines) -- so the desktop
 * reflects what the app declares, not a hard-coded table. Missing manifest or
 * field => the pre-seeded fallback stays. */
static int load_app_meta(const char *name, char *icon, size_t ic, char *label, size_t lc) {
    char path[128];
    snprintf(path, sizeof path, "/data/apps/%s/%s.app", name, name);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return 0;
    for (size_t i = 0; i < len; ) {
        while (i < len && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i >= len) break;
        if (buf[i] == '#') { while (i < len && buf[i] != '\n') i++; continue; }
        size_t ks = i;
        while (i < len && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        size_t kl = i - ks;
        while (i < len && (buf[i]==' '||buf[i]=='\t')) i++;
        size_t vs = i;
        while (i < len && buf[i]!='\n' && buf[i]!='\r') i++;
        size_t vl = i - vs;
        while (vl && (buf[vs+vl-1]==' '||buf[vs+vl-1]=='\t')) vl--;
        char *dst = 0; size_t cap = 0;
        if (kl==4 && !memcmp(buf+ks,"name",4)) { dst = label; cap = lc; }
        else if (kl==4 && !memcmp(buf+ks,"icon",4)) { dst = icon; cap = ic; }
        if (dst && cap) { size_t n = vl < cap-1 ? vl : cap-1; memcpy(dst, buf+vs, n); dst[n] = 0; }
    }
    free(buf);
    return 1;
}

/* --- the Apps launcher: a grid of ALL installed apps, read from /data/apps ---
 * Each app self-describes via its <name>.app (name + icon); click to launch. */
struct grid_app { char name[32]; char icon[96]; char label[24]; char exec[80]; };
static struct grid_app g_all[48];
static int g_all_n = 0;
static int   g_apps_open = 0;     /* is the launcher grid showing? */
static int   g_apps_frames = 0;   /* debounce the click that opened it */
static float g_apps_scroll = 0;   /* launcher grid scroll offset */
static char  g_apps_q[40];        /* the launcher's search text */
/* Bumped every time the launcher opens; the sheet is keyed by it. See below. */
static unsigned g_apps_gen = 0;

/* Enumerate /data/apps/<name>/, keeping those that hold a <name>.elf, and read
 * each one's presentation manifest. home's namespace grants `ro /data/apps`. */
static void scan_apps(void) {
    g_all_n = 0;
    /* Internal tools share this directory. A 64-entry read silently hid
     * declared apps later in the directory, including Terminal. */
    static struct embk_dirent ents[256];
    int64_t n = embk_readdir("/data/apps", ents, 256);
    for (int64_t i = 0; i < n && g_all_n < 48; i++) {
        if (ents[i].type != EMBK_DT_DIR) continue;
        const char *nm = ents[i].name;
        if (nm[0] == '.') continue;
        struct grid_app *a = &g_all[g_all_n];
        snprintf(a->exec, sizeof a->exec, "/data/apps/%s/%s.elf", nm, nm);
        struct embk_stat st;
        if (embk_stat(a->exec, &st) < 0) continue;   /* only real, launchable apps */
        snprintf(a->name, sizeof a->name, "%s", nm);
        snprintf(a->icon, sizeof a->icon, "/system/images/app_launcher.eic");  /* fallback */
        snprintf(a->label, sizeof a->label, "%s", nm);
        /* only apps that DECLARE themselves (ship a <name>.app) appear in the
         * launcher -- internal tools/tests stay out of the user's face. */
        if (!load_app_meta(nm, a->icon, sizeof a->icon, a->label, sizeof a->label)) continue;
        g_all_n++;
    }
}

/* --- drag-and-drop state (a desktop icon INTO the dock, or a dock icon OUT) --- */
static int   g_drag = 0;        /* 0 none, 1 dragging a desktop source, 2 a dock item */
static int   g_drag_i = -1;     /* index in g_desk (kind 1) or g_dock (kind 2) */
static struct app_item g_drag_item;
static float g_drag_sx, g_drag_sy;   /* press-start pointer */
static float g_drag_x, g_drag_y;     /* live pointer */
static int   g_drag_moved = 0;       /* travelled past the click threshold -> a real drag */
static int   g_any_active = 0;       /* any draggable held this frame */
static float g_dockr[4];             /* dock pill world rect: x0,y0,x1,y1 */
static int   g_have_dockr = 0;
static int   g_dock_dirty = 0;
/* Deferred out of the menu callback: the picture must be taken with the menu
 * GONE, or the screenshot is a photograph of the menu that asked for it. */
static int   g_want_shot = 0;
static int   g_shot_delay = 0;       /* dock changed -> the loop force-repaints (no ghosts) */
/* desktop context menu (right-click on empty desktop) */
static bool  g_ctx_open = false;
static float g_ctx_x, g_ctx_y;

static char  g_pin_exec[16][80];     /* stable exec strings for launcher apps pinned to the dock */
static int   g_pin_n = 0;

/* The top bar (a separate program) asks us to open the launcher over an IPC
 * channel: home LISTENS at /run/emlink.desktop on a background thread (accept
 * blocks, so it can't live in the render loop) and just raises a flag; the top
 * bar CONNECTS to signal. The render loop consumes the flag non-blockingly. */
static volatile int g_apps_requested = 0;
static void apps_listener(long arg) {
    (void)arg;
    int lh = (int)embk_chan_listen("/run/emlink.desktop");
    if (lh < 0) {
        /* Without this endpoint the top bar's launcher button can do nothing,
         * and it used to exit here without a word -- the one line that would
         * have explained a dead button. */
        char b[96];
        snprintf(b, sizeof b, "home: cannot listen at /run/emlink.desktop (%d) -- "
                              "the launcher button will do nothing\n", lh);
        embk_puts(1, b);
        embk_thread_exit(1);
    }
    for (;;) {
        int ch = (int)embk_chan_accept(lh);
        if (ch < 0) {
            /* accept only BLOCKS while the endpoint is healthy; the error paths
             * (bad/unlinked handle, handle table full) return straight away, so
             * a bare retry here would be an unbounded hot spin -- on one core
             * that starves the render loop and the whole desktop freezes. */
            embk_sleep_ms(100);
            continue;
        }
        embk_puts(1, "home: launcher requested by the top bar\n");
        g_apps_requested = 1;          /* the render loop opens the launcher */
        embk_chan_close(ch);
    }
}
/* Open or close the launcher, and move the desktop LAYER with it.
 *
 * The layer is the ground at z=0, so a launcher drawn into it opens behind
 * every app window -- press the button with anything open and nothing appears
 * to happen, which is exactly what it did. Launchpad is in front, so the layer
 * comes to the front for as long as the launcher is up and goes back down the
 * moment it closes. Everything else about the desktop is unchanged; this is a
 * mode, not a reordering. */
/* Frames still owed a FULL present after the launcher opened or closed. The
 * transition changes every pixel on the layer, and it changes them in two
 * places that do not happen together: the compositor recomposes the moment the
 * z-flip lands, while this loop only renders the new state on its NEXT frame
 * and then presents the dirty rects of it. Anything the new frame does not
 * touch therefore keeps whatever the old one left -- the launcher's title
 * stayed painted across the top of the screen, over the menu bar. A whole-
 * surface present for the frames either side of the flip is what makes the two
 * agree. */
static int g_full_present = 0;

/* ---- THE SCREEN LOCK ---------------------------------------------------
 *
 * WHY THE DESKTOP DRAWS IT AND NOT A SEPARATE PROGRAM. A lock must be in front
 * of everything and must take every click and every keystroke. The desktop
 * already owns the one surface with that property: the layer at z=0 that
 * embk_win_desktop_front() lifts above every application window -- the same
 * mechanism the Applications launcher uses. A separate lock program would be
 * an ordinary window, and an ordinary window can be lowered.
 *
 * WHAT IT CANNOT DO IS CHECK THE PASSWORD. /etc/shadow is outside this
 * session's namespace, deliberately, so the check goes to authd over
 * /run/emlink.auth -- see user/lib/emauth.h for why that division is the whole
 * design and not an inconvenience.
 *
 * WHAT THIS IS NOT. It is not a privilege boundary inside the session: every
 * process already running in it keeps running, keeps its files open, and could
 * draw over the screen if it wanted to. It is the answer to "someone walked
 * past the machine", which is the threat a screen lock is actually for. */
static int  g_locked;                  /* is the lock surface up? */
/* Frames on which the renderer's rect cache is thrown away, so it repaints
 * every pixel rather than what it believes changed.
 *
 * A whole-screen surface appearing or disappearing is the one change where
 * "repaint what moved" has nothing to work with and the cheap thing to do is
 * the whole screen. Two frames: the one that puts the lock up, and the one
 * after it, which is when the layer's z-flip has landed in the compositor. */
static int  g_invalidate;
static int  g_lock_req;                /* the menu bar asked for one */
static int  g_lock_checking;           /* a verify is in flight on its own thread */
static int  g_lock_result;             /* what it came back with (EMAUTH_*) */
static int  g_lock_done;               /* ...and that it came back at all */
static char g_lock_pw[EMBK_AUTH_PASSWORD_MAX + 1];
static char g_lock_msg[96];
static char g_lock_user[EMBK_AUTH_USERNAME_MAX + 1];
static unsigned g_lock_gen;            /* keys the subtree; see apps_grid's comment */

/* The menu bar's Lock signal. Its own endpoint rather than a payload on the
 * launcher's: the connect IS the message there, and adding a first byte to it
 * would make every existing client's silence mean something. */
static void lock_listener(long arg) {
    (void)arg;
    int lh = (int)embk_chan_listen("/run/emlink.lock");
    if (lh < 0) {
        char b[96];
        snprintf(b, sizeof b, "home: cannot listen at /run/emlink.lock (%d) -- "
                              "the menu bar's Lock Screen will do nothing\n", lh);
        embk_puts(1, b);
        embk_thread_exit(1);
    }
    for (;;) {
        int ch = (int)embk_chan_accept(lh);
        if (ch < 0) { embk_sleep_ms(100); continue; }
        g_lock_req = 1;
        embk_chan_close(ch);
    }
}

/* THE CHECK RUNS ON ITS OWN THREAD because a wrong password is SUPPOSED to be
 * slow -- authd waits out an escalating penalty before answering, up to thirty
 * seconds. Done on the render thread that would be thirty seconds of a frozen
 * screen, which looks exactly like a crash and is also a lie: the machine is
 * fine, it is refusing you on purpose. */
static void lock_verify_thread(long arg) {
    (void)arg;
    struct emauth_rep rep;
    int v = emauth_verify(g_lock_pw, &rep);
    for (unsigned i = 0; i < sizeof g_lock_pw; i++) g_lock_pw[i] = 0;   /* not kept */
    g_lock_result = v;
    g_lock_done = 1;
    embk_thread_exit(0);
}

static void lock_set(int on) {
    if (on == g_locked) return;
    int gate=embk_session(on?EMBK_SESSION_LOCK:EMBK_SESSION_UNLOCK,0);
    if(gate<0){embk_puts(1,"home: lock ownership request failed\n");return;}
    g_locked = on;
    if (on) {
        ui_request_focus(INSTANCE_HANDLE_NULL); /* password owns the new modal */
        g_apps_open = 0;                      /* one modal surface at a time */
        g_lock_gen++;
        g_lock_msg[0] = 0;
        for (unsigned i = 0; i < sizeof g_lock_pw; i++) g_lock_pw[i] = 0;
    }
    /* The launcher may have owned the layer; the lock does now, and when it
     * goes the layer goes back to the ground unless the launcher still wants
     * it -- which it cannot, because locking closed it. */
    int rc = embk_win_desktop_front(on || g_apps_open);
    em_structure_changed();
    g_full_present = 3;
    g_invalidate = 2;
    g_dock_dirty = 1;
    { char b[96];
      snprintf(b, sizeof b, "home: screen %s (desktop_front rc=%d)\n",
               on ? "LOCKED" : "unlocked", rc);
      embk_puts(1, b); }
}

/* Asked for a lock. Refused, out loud, when there is nothing to unlock with:
 * the development auto-login opens a session for an account the store has
 * never heard of, and putting a password prompt in front of that would lock
 * the machine until it is rebooted. */
static void poll_lock_request(void) {
    int state=embk_session(EMBK_SESSION_STATUS,0);
    if(state>=0 && (state&EMBK_SESSION_LOCKED) && !g_locked)g_lock_req=1;
    if (!g_lock_req) return;
    g_lock_req = 0;
    if (g_locked) return;
    if(state>=0 && (state&EMBK_SESSION_LOCKED)) {
        struct embk_session_info si;
        if(!embk_session_info(&si))snprintf(g_lock_user,sizeof g_lock_user,"%s",si.user);
        lock_set(1);return;
    }

    struct emauth_rep rep;
    int st = emauth_status(&rep);
    if (st == EMAUTH_NOACCOUNT) {
        embk_notify_level(EMNOTE_WARN, "Lock Screen",
                          "This account has no password, so there would be no way "
                          "to unlock. Set one from the login screen first.");
        embk_puts(1, "home: lock refused -- this session's user has no account\n");
        return;
    }
    if (st < 0) {
        embk_notify_level(EMNOTE_FAIL, "Lock Screen",
                          "The password service is not running; the screen cannot "
                          "be locked.");
        char b[96];
        snprintf(b, sizeof b, "home: lock refused -- no authd (%d)\n", st);
        embk_puts(1, b);
        return;
    }
    snprintf(g_lock_user, sizeof g_lock_user, "%s", rep.user);
    lock_set(1);
}



static void apps_set_open(int open) {
    if (open == g_apps_open) return;
    if (open) { ui_request_focus(INSTANCE_HANDLE_NULL); scan_apps(); g_apps_frames = 0; g_apps_scroll = 0; g_apps_q[0] = 0; g_apps_gen++; }
    g_apps_open = open;
    /* Render the new state BEFORE moving the layer when closing, and move it
     * before rendering when opening -- either way the compositor's recompose
     * must not be the one that shows a half-updated desktop. */
    /* In front of the app windows. Safe now that the layout arena unlinks a
     * destroyed node from its parent: until it did, the launcher's second
     * appearance was never measured, so lifting the layer only put a blank
     * full-screen surface over everything and took the menu bar with it. */
    int rc = embk_win_desktop_front(open);
    /* TELL THE RUNTIME THE TREE CHANGED SHAPE. The launcher is a whole subtree
     * that appears and disappears, and the renderer only repaints what it can
     * see has changed -- so the SECOND time it opened, with the same apps in
     * the same order, it rebuilt an identical subtree, nothing registered as a
     * mutation, and not one pixel of it was painted. The layer came to the
     * front (hiding the menu bar, which is how it looked like a crash) over a
     * desktop that had simply not drawn its launcher. */
    em_structure_changed();
    g_invalidate = 2; /* Home owns its loop; it must consume structural invalidation. */
    g_full_present = 3;
    g_dock_dirty = 1;
    /* Say so on the serial console. The launcher is reachable only from the
     * menu bar once an app covers the screen, and a synthesized click cannot
     * work that button -- so when it misbehaves this line is the only way to
     * tell "the request never arrived" from "it opened and something closed
     * it again". */
    { char b[96];
      snprintf(b, sizeof b, "home: launcher %s (%d apps, desktop_front rc=%d)\n",
               open ? "OPEN" : "close", g_all_n, rc);
      embk_puts(1, b); }
}

static void poll_apps_request(void) {
    if (!g_apps_requested) return;
    g_apps_requested = 0;
    /* OPEN, not toggle. Toggling reads better and is wrong here: the signal is
     * a bare IPC connect with no payload, so anything that produces two of them
     * for one press -- a repeat, a reconnect, the bar's own 1Hz frame seeing
     * the click twice -- opens the launcher and closes it again in the same
     * breath, which looks exactly like a button that does nothing. Dismissing
     * has three ways already: Esc, click-off, and launching something. */
    apps_set_open(1);
}

static void open_item(struct app_item it) {
    if (it.dir)      launch_folder(it.dir);   /* a folder shortcut -> Files there */
    else if (it.app) g_launch = it.app;       /* an app -> spawn it */
}

/* Adapt a launcher-grid entry to the draggable app_item shape. The exec path
 * points at g_all[]'s reusable buffer; a drop that pins it copies the string to
 * stable storage (dock_resolve_drop), so a later rescan can't alias it. */
static struct app_item grid_to_item(int i) {
    struct app_item it; memset(&it, 0, sizeof it);
    snprintf(it.icon,  sizeof it.icon,  "%s", g_all[i].icon);
    snprintf(it.label, sizeof it.label, "%s", g_all[i].label);
    it.app = g_all[i].exec;
    return it;
}

/* One draggable icon. kind: 1 = desktop source, 2 = dock item, 3 = launcher grid.
 * Renders the icon
 * (with hover styling) and tracks a press as either a click or a drag; the drop
 * is resolved after the frame in dock_resolve_drop(). */
static Color    g_desk_ink = { .r = .93f, .g = .94f, .b = .96f, .a = 1.f };

/* Colour tiles have opaque backgrounds, so alpha-tinting the whole tile
 * erases its glyph. Monochrome uses the matching original line silhouette. */
static const char *monochrome_icon(const char *path) {
    static const struct {const char *file,*symbol;} family[]={
        {"files.eic","/system/images/ui-folder.eic"},
        {"terminal.eic","/system/images/ui-terminal.eic"},
        {"setting.eic","/system/images/ui-gear.eic"},
        {"editor.eic","/system/images/ui-edit.eic"},
        {"notepp.eic","/system/images/ui-document.eic"},
        {"photos.eic","/system/images/ui-photo.eic"},
        {"music.eic","/system/images/ui-music.eic"},
        {"monitor.eic","/system/images/ui-monitor.eic"},
        {"calc.eic","/system/images/ui-calculator.eic"},
        {"vellum.eic","/system/images/ui-window.eic"},
        {"netsurf.eic","/system/images/ui-network.eic"},
        {"file.eic","/system/images/ui-document.eic"},
        {"notifications.eic","/system/images/ui-bell.eic"},
        {"launcher.eic","/system/images/ui-launcher.eic"},
        {"app_launcher.eic","/system/images/ui-grid.eic"}
    };
    const char *file=strrchr(path,'/');file=file?file+1:path;
    for(unsigned i=0;i<sizeof family/sizeof family[0];i++)
        if(!strcmp(file,family[i].file))return family[i].symbol;
    return "/system/images/ui-grid.eic";
}

static void drag_icon(struct app_item it, int size, int kind, int idx) {
    /* Key by the app's IDENTITY (its unique path), NOT its slot index. When the
     * list shifts (remove/reorder), an index-keyed instance gets reused for a
     * different app -- which left a removed middle app lingering (dimmed) in
     * place while the pill kept its old width. Identity keys track each app. */
    uint64_t base = kind == 1 ? 0xDE510000ULL : kind == 3 ? 0x6D170000ULL : 0xD0C00000ULL;
    uint64_t key  = base ^ (uint64_t)(uintptr_t)(it.app ? it.app : it.dir);
    ui_box_begin(key);
    (void)ui_open();
    ui_set_align(ALIGN_CENTER);
    /* ALWAYS set opacity, so an instance never stays dimmed after its drag ends */
    ui_set_opacity((g_drag == kind && g_drag_i == idx && g_drag_moved) ? 0.3f : 1.0f);
    if(ui_theme()->icon_theme)ImageButtonTinted(monochrome_icon(it.icon),size,kind==1?g_desk_ink:ui_theme()->text);
    else ImageButton(it.icon, size);         /* styling only; click/drag handled below */
    if (ui_is_active()) {
        g_any_active = 1;
        float px, py; ui_pointer_pos(&px, &py);
        if (!(g_drag == kind && g_drag_i == idx)) {  /* new press -> start tracking */
            g_drag = kind; g_drag_i = idx; g_drag_item = it;
            g_drag_sx = px; g_drag_sy = py; g_drag_moved = 0;
        }
        g_drag_x = px; g_drag_y = py;
        if (!g_drag_moved) {
            float dx = px - g_drag_sx, dy = py - g_drag_sy;
            if (dx*dx + dy*dy > 49.0f) g_drag_moved = 1;    /* > ~7px = a drag */
        }
    }
    ui_box_end();
}

/* The dock: a centered pill sized to its apps. Captures its own world rect so
 * the drop can hit-test against it. */
static int dock_running(const char *path);   /* below, with g_running */
static void wins_poll(void);                 /* likewise: the live window list */

/* THE DOCK DOES NOT REACT TO THE POINTER. It reacts to the MACHINE.
 *
 * What was here was cursor-driven magnification -- an icon's size as a pure
 * function of the pointer's horizontal distance to it. That is the Mac dock's
 * defining gesture, it was implemented here as that, and its comment said so.
 * Keeping it would have made every other difference cosmetic.
 *
 * What replaces it is the idea this dock is built on: EmbLinkOS is an
 * operating system that measures itself -- per-core idle residency, a
 * scheduler policy you can name, a page cache that reports its own hit rate --
 * and its dock should show the machine, not the hand. So:
 *
 *   * the icons never move or swell. A dock is a target, and a target that
 *     changes size under the pointer is a worse target (the Mac pays this
 *     price knowingly for the delight; we are not copying the delight, so we
 *     do not owe the price);
 *   * a RUNNING app sits in a lit SOCKET -- a filled, hairline-framed plate
 *     behind its icon. Not a dot underneath: the dot is a footnote, and
 *     whether an app is running is the single most useful thing this strip
 *     knows. A socket is visible from across the room;
 *   * the pointer gets a quiet backplate, nothing more.
 *
 * What has NOT changed: the floating label that names the icon under the
 * pointer. Every dock ever built names what you point at, because dock art is
 * abstract everywhere and ours more than most -- see dock_label(). */


static struct oscfg g_cfg;
static uint64_t g_cfg_next=0;
static struct oscfg g_cfg_published;
static embk_mutex g_cfg_mutex=EMBK_MUTEX_INIT;

/* REDRAW EVERYTHING, NOW. For a change the renderer's dirty tracking cannot
 * see: a theme whose colours moved under an unchanged tree, or a background
 * image that is a different picture at the same node. em_structure_changed()
 * tells the runtime not to trust last frame's diff; the three frames of full
 * presentation are what actually push the pixels, because the compositor
 * presents dirty rects and there are none to present. */
static void force_repaint(void) {
    em_structure_changed();
    g_full_present = 3;
    g_dock_dirty = 1;
}
static void cfg_poll(void) {
    uint64_t now = embk_uptime_ms();
    if (now < g_cfg_next) return;
    g_cfg_next = now + 1000;
    struct oscfg was = g_cfg;
    oscfg_load(&g_cfg);
    static int preferences_established;
    if(!preferences_established) {
        preferences_established=1;
        struct embk_stat st;
        if(embk_stat(oscfg_path(),&st)<0)oscfg_save(&g_cfg);
    }
    if(g_cfg.volume!=was.volume || g_cfg.muted!=was.muted)embk_audio_volume(g_cfg.volume,g_cfg.muted);
    if(g_cfg.idle_dim_seconds!=was.idle_dim_seconds || g_cfg.idle_lock_seconds!=was.idle_lock_seconds ||
       g_cfg.lid_lock!=was.lid_lock || g_cfg.power_profile!=was.power_profile) {
        struct embk_policy policy={.version=EMBK_POLICY_VERSION,.flags=g_cfg.lid_lock?EMBK_POLICY_LID_LOCK:0,
            .idle_dim_seconds=g_cfg.idle_dim_seconds,.idle_lock_seconds=g_cfg.idle_lock_seconds,.profile=g_cfg.power_profile};
        embk_policy_set(&policy);
    }

    /* The pieces of the shell that cannot name the user's home ask us for the
     * preferences over /run/emlink.prefs; g_cfg is what we answer with, and the
     * server thread below reads it. Nothing to push. */

    /* The shell wears the preference too, and live. em_app_run applies it once
     * at launch for ordinary applications; the desktop never exits, so if it
     * only read the file at startup then changing the accent would recolour
     * every window EXCEPT the one always on screen. */
    /* THE KEYBOARD LAYOUT IS APPLIED BY THE SHELL, once at start and again
     * whenever the preference changes. It has to be asked for: the layout is
     * the DRIVER's policy, so the saved file records what the machine should
     * type and only this call makes it actually type that. First poll counts
     * as a change (`first`), which is what makes the setting survive a reboot
     * rather than needing to be re-picked every session. */
    static int first = 1;
    if (first || g_cfg.keymap != was.keymap) {
        first = 0;
        int k = (g_cfg.keymap >= 0 && g_cfg.keymap < OSCFG_KEYMAPS) ? g_cfg.keymap : 0;
        embk_kbd_layout(oscfg_keymaps[k].name, 0, 0);
    }

    static int appearance_applied;
    if(!appearance_applied || memcmp(&g_cfg,&was,sizeof g_cfg)) {
        oscfg_apply_appearance(&g_cfg);
        /* A lock can temporarily gate publication. Retry the same snapshot
         * after ownership returns instead of leaving chrome/insets stale. */
        appearance_applied=oscfg_publish_desktop(&g_cfg)>=0;force_repaint();
    }

    embk_mutex_lock(&g_cfg_mutex);g_cfg_published=g_cfg;embk_mutex_unlock(&g_cfg_mutex);

    /* A NEW DESKTOP PICTURE REPAINTS NOTHING BY ITSELF. The renderer draws what
     * it can see has CHANGED, and swapping the path a BackgroundImage points at
     * builds an identical tree -- same nodes, same shape, same everything but
     * the bytes behind one of them. So the preference was stored, Settings
     * showed the new picture in its preview, and the desktop behind it kept
     * the old one indefinitely: three parties, two of them right.
     *
     * Measured, which is the only reason this line exists: the wallpaper test
     * reported the strip of desktop beside the window changing 0.0%. */
    if (g_cfg.wallpaper != was.wallpaper)
        force_repaint();

    /* The zone is applied by oscfg_load() -- this only SAYS so, once, because
     * a clock that is an hour out is the kind of wrong a person blames on the
     * clock rather than on a setting they changed. Applications launched from
     * here get it in their environment; ones already running keep the zone
     * they were started with, which is why this line names the change. */
    static int tz_first = 1;
    if (tz_first || g_cfg.tz != was.tz) {
        tz_first = 0;
        printf("home: time zone %s (TZ=%s)\n", oscfg_tz_label(&g_cfg),
               oscfg_tz_string(&g_cfg));
        fflush(stdout);
    }
}

/* THE PREFERENCES, FOR WHOEVER CANNOT READ THE FILE.
 *
 * The menu bar's namespace is `ro /system, rw /run` and its manifest means it.
 * It cannot open ~/settings.conf and should not be handed the authority to, so
 * the desktop -- which can -- answers for it.
 *
 * The endpoint's whole meaning is "connect and I will hand you the
 * preferences": no request payload, symmetric with the launcher endpoint
 * beside it where the connect is itself the signal. That symmetry is not
 * tidiness; it is what keeps this thread from ever BLOCKING ON A READ. A
 * server that waits for a client to say what it wants is a server one rude
 * client can wedge, and this one is single-threaded and sits behind the
 * launcher button.
 *
 * A published file under /run would have been simpler and is not possible:
 * /run is epfs, which holds endpoint nodes and nothing else. Measured, as
 * `home: publish rc=-1`, before this existed. */
static void prefs_listener(long arg) {
    (void)arg;
    int lh = (int)embk_chan_listen(OSCFG_ENDPOINT);
    if (lh < 0) {
        char b[112];
        snprintf(b, sizeof b, "home: cannot listen at %s (%d) -- the menu bar "
                              "will run on default preferences\n", OSCFG_ENDPOINT, lh);
        embk_puts(1, b);
        embk_thread_exit(1);
    }
    for (;;) {
        int ch = (int)embk_chan_accept(lh);
        if (ch < 0) { embk_sleep_ms(100); continue; }
        struct oscfg snapshot;
        embk_mutex_lock(&g_cfg_mutex);snapshot=g_cfg_published;embk_mutex_unlock(&g_cfg_mutex);
        oscfg_serve_one(ch,&snapshot);
        embk_chan_close(ch);
    }
}


/* The dock's base size is a PREFERENCE, not a constant: Settings writes it and
 * the desktop re-reads it about once a second, so the dock changes under the
 * slider instead of after a reboot. */
#define DOCK_BASE ((float)g_cfg.dock_size)
/* Dock band geometry. These were implicit and they DISAGREED: the row reserved
 * 64px while the pill inside it was 70 tall, so the pill overflowed its row
 * and the last 6px -- its bottom edge and rounded corners -- fell off the
 * bottom of the screen. One set of constants now, and the band is the pill
 * PLUS a gap, because a dock that touches the screen edge is a taskbar; the
 * gap is what makes it read as floating. */
#define DOCK_PILL_H (DOCK_BASE + 32.0f)
#define DOCK_GAP    14.0f
/* ONE formula, in oscfg.h, because this number is also what every app window
 * must stay out of -- and when the two disagreed, app windows covered the top
 * of the dock and swallowed its clicks. A contract in a shared header cannot
 * drift the way two copies of a constant did. */
#define DOCK_BAND   ((float)oscfg_dock_band(&g_cfg))
#define BAR_RESERVE ((float)g_cfg.bar_size)      /* must match topbar.c's BAR_H */

/* WHAT COLOUR A DESKTOP LABEL SHOULD BE, which is the WALLPAPER's business and
 * not the theme's.
 *
 * A caption inside a window is dark on a light surface and light on a dark
 * one, and the theme knows which. A desktop icon's caption sits on the
 * picture, where the theme knows nothing: switch to the light theme over a
 * dark wallpaper and every label turns near-black on near-black and simply
 * disappears. It is not a subtle bug -- the "System" label vanished the
 * instant the theme changed, while the pixels behind it had not moved.
 *
 * Same answer the menu bar already uses, for the same reason: ask what is
 * actually composed there. The sample is the strip directly ABOVE the first
 * icon -- wallpaper the desktop never draws on, so reading it cannot end up
 * measuring last frame's own text. Twice a second; a wallpaper does not change
 * at frame rate and reading pixels is not free.
 *
 * The two thresholds are hysteresis. One would make a label flicker between
 * inks on a wallpaper that happens to sit near it. */
static int      g_desk_ink_dark = 0;
static uint64_t g_desk_ink_next = 0;
static void desk_ink_poll(void) {
    uint64_t now = embk_uptime_ms();
    if (now < g_desk_ink_next) return;
    g_desk_ink_next = now + 500;
    int l = embk_screen_luma(0, (int)BAR_RESERVE, DESK_CELL_W * 2, 8);
    if (l < 0) return;
    if (!g_desk_ink_dark && l > 150)      g_desk_ink_dark = 1;
    else if (g_desk_ink_dark && l < 120)  g_desk_ink_dark = 0;
    g_desk_ink = g_desk_ink_dark ? (Color){ .r = .09f, .g = .09f, .b = .10f, .a = 1.f }
                                 : (Color){ .r = .93f, .g = .94f, .b = .96f, .a = 1.f };
}

/* Where slot `i` ACTUALLY starts, derived from the layout rather than guessed.
 *
 * The row is: px 12, then per slot a VStack of width DOCK_BASE+8 separated by
 * spacing 10 -- so the pitch is DOCK_BASE+18, not DOCK_BASE+10. Three places
 * had independently written the +10 version, and the error compounds with the
 * slot index: at the default size it is 8px off at slot 1 and 24px off at slot
 * 3. Proven, not guessed -- the dock's captured world rect is 238px wide for
 * four slots, and 4*(38+8) + 3*10 + 2*12 is exactly 238, while the +10 formula
 * predicts 216.
 *
 * The visible cost, when it was wrong: the label named the wrong app and the
 * hover plate lit the wrong socket. */
#define DOCK_SLOT_W  (DOCK_BASE + 8.0f)
#define DOCK_PITCH   (DOCK_SLOT_W + 10.0f)
static float dock_slot_x0(int i) { return g_dockr[0] + 12.0f + (float)i * DOCK_PITCH; }

/* WHERE EACH TILE IS, told to the compositor: a minimised window flies into
 * its app's tile and back out of it, not toward a guess at the bottom of the
 * screen. The same geometry the hover test below uses, so the window lands on
 * the tile the pointer would light. Said again only when it changes. */
static void dock_publish_targets(void) {
    static struct embk_dock_target last[EMBK_DOCK_TARGETS_MAX];
    static int last_n = -1;
    struct embk_dock_target t[EMBK_DOCK_TARGETS_MAX];
    int n = 0;
    for (int i = 0; i < g_dock_n && n < (int)EMBK_DOCK_TARGETS_MAX; i++) {
        if (!g_dock[i].app) continue;
        memset(&t[n], 0, sizeof t[n]);
        strncpy(t[n].app, embk_basename(g_dock[i].app), sizeof t[n].app - 1);
        float along = g_cfg.dock_edge ? g_dockr[1] + 12 + i * DOCK_PITCH : dock_slot_x0(i);
        float across = g_cfg.dock_edge ? (g_dockr[0] + g_dockr[2] - DOCK_SLOT_W) * .5f
                                       : (g_dockr[1] + g_dockr[3] - DOCK_SLOT_W) * .5f;
        t[n].x = (int32_t)(g_cfg.dock_edge ? across : along);
        t[n].y = (int32_t)(g_cfg.dock_edge ? along : across);
        t[n].w = t[n].h = (int32_t)DOCK_SLOT_W;
        n++;
    }
    if (n == last_n && !memcmp(t, last, (size_t)n * sizeof t[0])) return;
    if (embk_dock_targets(t, (unsigned)n) == 0) { memcpy(last, t, (size_t)n * sizeof t[0]); last_n = n; }
}

/* Which slot the pointer is over, or -1. The readout and the hover plate both
 * ask this, so they can never disagree about which app you mean. */
static int dock_hover_index(void) {
    if (!g_have_dockr || g_drag) return -1;
    float px, py; ui_pointer_pos(&px, &py);
    if(px<g_dockr[0] || px>g_dockr[2] || py<g_dockr[1] || py>g_dockr[3])return -1;
    for(int i=0;i<g_dock_n;i++) {
        float origin=g_cfg.dock_edge?g_dockr[1]+12+i*DOCK_PITCH:dock_slot_x0(i);
        float pos=g_cfg.dock_edge?py:px;
        if(pos>=origin && pos<origin+DOCK_SLOT_W)return i;
    }
    return -1;
}

/* The name of the icon under the pointer, floating above the dock.
 * Our dock art is abstract -- a glyph is a guess until something names it --
 * and a label that lives IN the dock would either cost permanent vertical
 * space or shift the icons when it appeared. So it is an out-of-flow overlay
 * (the drag-ghost pattern): it costs no layout and moves nothing. */
static void dock_label(void) {
    int i = dock_hover_index();
    if (i < 0 || !g_cfg.dock_labels || !g_dock[i].label[0]) return;
    float cx = dock_slot_x0(i) + DOCK_SLOT_W * 0.5f;
    /* rough centring: the toolkit measures the text, we only have its length,
     * and being a few pixels off is invisible next to a 46px icon */
    int n = 0; while (g_dock[i].label[n]) n++;
    float w = (float)n * 7.0f + 20.0f;

    em_flush();                    /* emit pending DSL leaves before raw ui_* */
    ui_begin_vstack(0x70CA);
    ui_set_overlay(true);
    ui_set_layer(1);
    /* ZERO-SIZED, for the reason desktop_icons() spells out: a screen-filling
     * transparent overlay CONTAINS every point, and the hit test gives the
     * highest layer that claims the point. This one only exists WHILE the
     * pointer is over a dock icon -- which is precisely when you are trying to
     * click one -- so at full size it swallowed every launch click. With no
     * extent, only the label's own little box exists for hit testing. */
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
    ui_set_clip_children(false);
    ui_begin_vstack(1);
    ui_set_offset(g_cfg.dock_edge==1?g_dockr[2]+8:g_cfg.dock_edge==2?g_dockr[0]-w-8:cx-w*.5f,
                  g_cfg.dock_edge?g_dockr[1]+12+i*DOCK_PITCH:g_dockr[1]-32);
    HStack(.px = 10, .py = 4, .corner = 9, .glass = 1, .border = 1) {
        Text(g_dock[i].label).caption();
    }
    ui_end_stack();
    ui_end_stack();
}

static void dock_pill(void) {
    const struct ui_theme *t = ui_theme();
    int hov = dock_hover_index();

    /* THE RACK. Not a pill: corner 14 against the pill's 20, which is the
     * theme's own radius_lg -- the same curve every card and panel in the OS
     * uses. A dock cut from the system's shape language belongs to the system;
     * a pill belongs to whoever invented that pill.
     *
     * Still glass, still floating clear of the screen edge (a dock that
     * touches the edge is a taskbar), still bottom-aligned. */
    HStack(.height = DOCK_PILL_H, .spacing = 10, .px = 12, .pb = 6, .align = Center,
           .glass = 1, .corner = 14, .border = 1, .shadow = 2) {
        if(g_cfg.dock_edge) {
            ui_set_axis(AXIS_COLUMN);
            ui_set_size((struct layout_size){.mode=SIZE_FIXED,.fixed_value=DOCK_PILL_H},
                        (struct layout_size){.mode=SIZE_INTRINSIC});
            ui_set_padding(12,6,12,6);
        }
        (void)ui_open();
        { float x, y, w, h; g_have_dockr = ui_open_rect(&x, &y, &w, &h);
          if (g_have_dockr) { g_dockr[0]=x; g_dockr[1]=y; g_dockr[2]=x+w; g_dockr[3]=y+h; dock_publish_targets(); } }
        for (int i = 0; i < g_dock_n; i++) {
            int running = dock_running(g_dock[i].app);
            int over    = (i == hov) && !g_drag;

            /* THE SOCKET: the plate an app sits in, LIT while it runs.
             *
             * This replaces a 4px dot under the icon. The dot is a footnote,
             * and whether an app is running is the most useful thing this
             * strip knows -- worth the whole tile, not four pixels of it.
             * (`dock_dots` still governs it: the preference is "show me what
             * is running", and it now answers that question better.)
             *
             * Three states, and EVERY prop is set in all three. A reused
             * instance keeps whatever the new state fails to restate, which is
             * exactly how the old dot survived the app that owned it -- see
             * em_apply_box, where both the fill and the border now honour an
             * explicit zero so that "off" can be said out loud. */
            Color plate  = { 0, 0, 0, 0 };
            Color edge   = { 0, 0, 0, 0 };
            float edge_w = EmZero;
            if (running && g_cfg.dock_dots) {
                plate  = t->surface_alt;
                edge   = t->border_strong;
                edge_w = 1;
            }
            if (over) {
                plate  = t->surface_alt;
                edge   = running && g_cfg.dock_dots ? t->accent : t->border_strong;
                edge_w = 1;
            }

            /* FIXED-width slot: the tile never moves and never resizes, so the
             * press you aimed is the press that lands. (The Mac trades this
             * away for magnification on purpose; we are not copying the
             * magnification, so we keep the target.) */
            VStack(.width = DOCK_SLOT_W, .height = DOCK_SLOT_W + 6, .align = Center,
                   .justify = Center, .corner = 10, .background = plate,
                   .border = edge_w, .border_color = edge) {
                drag_icon(g_dock[i], (int)DOCK_BASE, 2, i);
                HStack(.width=running && g_cfg.dock_dots?12:1,.height=2,.background=running && g_cfg.dock_dots?t->accent:T.clear,.corner=1) {}
            }
        }
        if (g_dock_n == 0) { EmProps hp = {0}; (void)hp; Text("  drag apps here  ").caption().secondary(); }
    }
}

/* The ghost: the dragged icon following the cursor (an out-of-flow overlay). */
static void drag_ghost(void) {
    if (!(g_drag && g_drag_moved)) return;
    const struct ui_theme *t=ui_theme();
    ui_begin_vstack(0x6057);ui_set_overlay(true);ui_set_layer(2);ui_set_hit_passthrough(true);
    ui_begin_vstack(1);ui_set_offset(g_drag_x-22,g_drag_y-22);ui_set_opacity(.85f);
    ui_set_padding(t->icon_theme?4:0,t->icon_theme?4:0,t->icon_theme?4:0,t->icon_theme?4:0);
    ui_set_paint((struct paint){.kind=PAINT_SOLID,.solid=t->icon_theme?t->surface:(Color){0,0,0,0}});
    ui_set_corner_radius(t->radius_sm);ui_set_border(t->icon_theme?1:0,t->border);
    if(t->icon_theme) {
        uint32_t w=0,h=0;const uint32_t *px=em_image_at(monochrome_icon(g_drag_item.icon),36,&w,&h);
        if(px)ui_image_sized_tinted(1,px,w,h,36,36,t->text);
    } else Image(g_drag_item.icon,.height=44);
    ui_end_stack();ui_end_stack();
}

/* The application library uses a bounded search/grid surface. Its dimensions
 * follow the work area, including side docks and either command-strip edge. */
/* The lock's wash is DARKER and far more opaque than the launcher's: the point
 * of the launcher is that you can still see your desktop behind it, and the
 * point of this is that you cannot. What is on your screen is as much yours as
 * what is in your files. */
static Color lock_tint(void) {
    Color c = { .r = 0.03f, .g = 0.04f, .b = 0.08f, .a = 1.0f };
    return c;
}
static Color lock_warn(void) {
    Color c = { .r = 0.96f, .g = 0.55f, .b = 0.45f, .a = 1.0f };
    return c;
}


/* Does this app match what has been typed? Case-insensitive substring, which
 * is the whole of "search" at this scale: with a dozen apps, ranking is a
 * feature nobody can see the benefit of. */
static int app_matches(const char *label, const char *q) {
    if (!q[0]) return 1;
    for (const char *a = label; *a; a++) {
        const char *x = a, *y = q;
        while (*x && *y) {
            char ca = *x | 32, cb = *y | 32;
            if (ca != cb) break;
            x++; y++;
        }
        if (!*y) return 1;
    }
    return 0;
}

/* The lock surface. Drawn from the same place in the tree as the launcher and
 * for the same reason -- last, so it is on top of everything the desktop
 * itself draws, while the LAYER being in front puts it over every app. */
static void lock_ui(void) {
    if (!g_locked) return;

    /* The verify thread's answer, collected on the render thread so nothing
     * below has to think about two threads touching the same strings. */
    if (g_lock_done) {
        g_lock_done = 0;
        g_lock_checking = 0;
        if (g_lock_result == EMAUTH_OK) {
            g_lock_msg[0] = 0;
            lock_set(0);
            return;
        }
        snprintf(g_lock_msg, sizeof g_lock_msg,
                 g_lock_result == EMAUTH_NOACCOUNT
                     ? "There is no account to check against."
                     : "That is not the password.");
        g_lock_gen++;            /* a fresh field, empty, focused */
    }

    char key[24];
    snprintf(key, sizeof key, "lock%u", g_lock_gen);
    Overlay() {
        /* OPAQUE, and not the Glass the launcher uses. The launcher is glass on
         * purpose -- you are meant to see the desktop you are choosing an app
         * to put on top of. A lock is the opposite: what is on your screen is
         * as much yours as what is in your files, and a wash you can read
         * through protects neither.
         *
         * It is also what MAKES it cover. The desktop's canvas is composited
         * PER PIXEL over the windows below it (compositor.c: "empty regions
         * reveal the desktop below"), so a surface at alpha 0.96 let the menu
         * bar's window -- 1016x340 while its dropdown is open, which is exactly
         * how a lock gets asked for -- show straight through the top of the
         * lock screen. Measured: the whole of that rectangle was still the
         * desktop, seconds later, hummingbird and all. */
        VStack(.width = g_sw, .height = g_sh, .align = Leading, .justify = Leading,
               .spacing = 0, .background = lock_tint(), .key = key) {
          at_begin(0x10C4CA2Dull, g_px, g_py);
          VStack(.width = g_pw, .height = g_ph, .align = Center, .justify = Center, .spacing = 0) {
            /* FILL, not Leading: a Leading card hugs its children, and a
             * password field that hugs its (empty) content is a 28-pixel box
             * -- which is what the first version drew. */
            Card(.width = 380, .px = 28, .py = 26, .spacing = 14, .align = Fill) {
                Text(g_datetime).caption().tertiary();
                Text(g_lock_user[0] ? g_lock_user : "Locked").heading();
                Text("Type your password to come back.").caption().secondary();
                if (g_lock_checking) {
                    HStack(.spacing = 10, .align = Center) {
                        Spinner();
                        Text("Checking...").caption().secondary();
                    }
                } else {
                    em_field_autofocus();
                    bool enter = PasswordField(g_lock_pw, sizeof g_lock_pw,
                                               "password").submitted();
                    if (Button("Unlock").primary().clicked() || enter) {
                        g_lock_msg[0] = 0;
                        g_lock_checking = 1;
                        g_lock_done = 0;
                        if (embk_thread_create(lock_verify_thread, 0) < 0) {
                            g_lock_checking = 0;
                            snprintf(g_lock_msg, sizeof g_lock_msg,
                                     "The check could not be started.");
                        }
                    }
                }
                if (g_lock_msg[0]) Text(g_lock_msg).caption().color(lock_warn());
            }
          }
          at_end();
        }
    }
    /* NO dismissal. The launcher closes when the scrim is clicked and Esc
     * closes it; this is the one surface in the system where neither may do
     * anything, and saying so here is cheaper than finding out later that a
     * key nobody thought about opens the machine. */
}

static void apps_grid(void) {
    if(!g_apps_open)return;
    g_apps_frames++;
    float width=g_pw-96;if(width>720)width=720;
    float height=g_ph-132;if(height>536)height=536;
    int cols=width<560?4:6;
    char sheet[24];snprintf(sheet,sizeof sheet,"apps%u",g_apps_gen);
    Overlay() {
        at_begin(0xA995EE7ull, g_px, g_py);
        VStack(.width=g_pw,.height=g_ph,.align=Center,.justify=Center,.key=sheet) {
            Glass(.width=width,.height=height,.padding=20,.spacing=14,.align=Fill,.corner=10,.shadow=3) {
                HStack(.spacing=12,.align=Center) {
                    Icon(IconGrid).color(T.accent);
                    Text("Applications").title();Spacer();
                    if(IconButton(IconClose).width(26).height(26).clicked())apps_set_open(0);
                }
                em_field_autofocus();
                if(SearchField(g_apps_q,sizeof g_apps_q,"Find an application").submitted()) {
                    for(int i=0;i<g_all_n;i++)if(app_matches(g_all[i].label,g_apps_q)) {
                        open_item(grid_to_item(i));apps_set_open(0);break;
                    }
                }
                Divider();
                ScrollView(&g_apps_scroll,height-158,.key="appsgrid") {
                    int count=0;
                    Grid(cols,.spacing=8) {
                        for(int i=0;i<g_all_n;i++) {
                            if(!app_matches(g_all[i].label,g_apps_q))continue;
                            count++;
                            VStack(.spacing=6,.align=Center,.py=5) {
                                drag_icon(grid_to_item(i),54,3,i);
                                Text(g_all[i].label).caption();
                            }
                        }
                    }
                    if(!count) {
                        Text("No applications found").body();
                        Text("Try a shorter name or clear the search.").caption().secondary();
                    }
                }
                Text("Drag an application to the dock to keep it close.").caption().secondary();
            }
        }
        at_end();
    }
    if(g_apps_frames>=3 && OverlayDismissed())apps_set_open(0);
}

/* --- free desktop placement -------------------------------------------------
 * Desktop icons are positioned individually rather than flowing in a list, so
 * one can be dropped anywhere instead of being pinned to its index. Layout only
 * assigns a spot to icons that have never been placed. */

/* Keep an icon fully inside the work area (below the top bar, above the dock). */
static void desk_clamp(struct app_item *it) {
    /* ...of the monitor it is on: the bars only take room on the primary. */
    float ox, oy, ow, oh;
    int prim = out_of(it->x + DESK_CELL_W / 2, it->y + DESK_CELL_H / 2, &ox, &oy, &ow, &oh);
    float minx = ox + DESK_MARGIN, miny = oy + (prim ? 40 : DESK_MARGIN);
    float maxx = ox + ow - DESK_CELL_W - DESK_MARGIN;
    float maxy = oy + oh - (prim ? 64 : 0) - DESK_CELL_H - DESK_MARGIN;
    if (maxx < minx) maxx = minx;
    if (maxy < miny) maxy = miny;
    if (it->x < minx) it->x = minx;
    if (it->y < miny) it->y = miny;
    if (it->x > maxx) it->x = maxx;
    if (it->y > maxy) it->y = maxy;
}

/* Give any unplaced icon a spot: down the left edge, wrapping into a new
 * column at the bottom -- the arrangement the desktop used to hard-code. */
static void desk_autoplace(void) {
    float x = g_px + DESK_MARGIN, y = g_py + 48;
    for (int i = 0; i < g_desk_n; i++) {
        if (g_desk[i].placed) continue;
        while (y + DESK_CELL_H > g_py + g_ph - 64 - DESK_MARGIN && x + DESK_CELL_W * 2 < g_px + g_pw) {
            x += DESK_CELL_W + 8; y = g_py + 48;   /* column full -> next column */
        }
        g_desk[i].x = x; g_desk[i].y = y; g_desk[i].placed = 1;
        desk_clamp(&g_desk[i]);
        y += DESK_CELL_H + 18;
    }
}

/* Icon positions outlive the session: a desktop that forgot where everything
 * was on every boot would not be worth arranging. Stored as one line per icon,
 * keyed by the app/folder path (the same stable identity the drag keys use). */
#define DESK_LAYOUT "/data/desktop.layout"

static void desk_save_layout(void) {
    char buf[1600]; int n = 0;
    for (int i = 0; i < g_desk_n && n < (int)sizeof buf - 160; i++) {
        const char *key = g_desk[i].app ? g_desk[i].app : g_desk[i].dir;
        if (!key) continue;
        n += snprintf(buf + n, sizeof buf - n, "%d %d %s\n",
                      (int)g_desk[i].x, (int)g_desk[i].y, key);
    }
    int fd = (int)embk_open(DESK_LAYOUT, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (fd < 0) return;                     /* read-only /data -> just don't persist */
    embk_write(fd, buf, (size_t)n);
    embk_close(fd);
}

static void desk_load_layout(void) {
    size_t len = 0;
    uint8_t *b = read_file(DESK_LAYOUT, &len);
    if (!b) return;
    for (size_t i = 0; i < len; ) {
        size_t ls = i;
        while (i < len && b[i] != '\n') i++;
        size_t le = i; if (i < len) i++;
        char line[256];
        size_t ll = le - ls; if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, b + ls, ll); line[ll] = 0;
        int x = 0, y = 0; char key[200];
        if (sscanf(line, "%d %d %199[^\n]", &x, &y, key) != 3) continue;
        for (int k = 0; k < g_desk_n; k++) {
            const char *kk = g_desk[k].app ? g_desk[k].app : g_desk[k].dir;
            if (kk && !strcmp(kk, key)) {
                g_desk[k].x = (float)x; g_desk[k].y = (float)y; g_desk[k].placed = 1;
                desk_clamp(&g_desk[k]);
                break;
            }
        }
    }
    free(b);
}

/* Resolve a released drag: add to / remove from / reorder the dock. Called after
 * the frame is built, when the dock rect and final pointer are known. */
static void dock_resolve_drop(void) {
    if (!g_drag || g_any_active) return;         /* still held, or nothing pressed */
    int over = g_have_dockr &&
               g_drag_x >= g_dockr[0] - 12 && g_drag_x <= g_dockr[2] + 12 &&
               g_drag_y >= g_dockr[1] - 28 && g_drag_y <= g_dockr[3] + 12;
    if (!g_drag_moved) {
        open_item(g_drag_item);                   /* a plain click -> launch/open */
        if (g_drag == 3) apps_set_open(0);        /* grid click closes launcher */
    } else if (g_drag == 3) {                      /* launcher app dragged toward the dock */
        /* pin a COPY: the grid's exec buffer gets reused on the next rescan, so
         * the dock keeps its own stable exec string. Skip if already docked. */
        int dup = 0;
        for (int k = 0; k < g_dock_n; k++)
            if (g_dock[k].app && g_drag_item.app && !strcmp(g_dock[k].app, g_drag_item.app)) dup = 1;
        if (over && !dup && g_dock_n < 16 && g_pin_n < 16) {
            char *buf = g_pin_exec[g_pin_n++];
            snprintf(buf, sizeof g_pin_exec[0], "%s", g_drag_item.app);
            g_drag_item.app = buf;
            g_dock[g_dock_n++] = g_drag_item;
            apps_set_open(0);                      /* pinned -> close the launcher */
            g_dock_dirty = 1;
        }                                          /* dropped off the dock -> launcher stays open */
    } else if (g_drag == 1) {                      /* a desktop icon */
        if (over && g_dock_n < 16) {
            g_dock[g_dock_n++] = g_drag_item;      /* onto the dock -> pin it ... */
            for (int k = g_drag_i; k < g_desk_n - 1; k++) g_desk[k] = g_desk[k+1];
            g_desk_n--;                            /* ... and REMOVE from the desktop */
            g_dock_dirty = 1;
            desk_save_layout();
        } else if (!over && g_drag_i >= 0 && g_drag_i < g_desk_n) {
            /* dropped on open desktop -> it LIVES there now. Centre the cell on
             * the cursor so the icon lands where the pointer let go, not offset
             * by wherever inside the icon the drag happened to start. */
            g_desk[g_drag_i].x = g_drag_x - DESK_CELL_W / 2.0f;
            g_desk[g_drag_i].y = g_drag_y - DESK_ICON / 2.0f;
            g_desk[g_drag_i].placed = 1;
            desk_clamp(&g_desk[g_drag_i]);
            g_dock_dirty = 1;
            desk_save_layout();
        }
    } else if (g_drag == 2) {                       /* dock item */
        if (!over) {                                /* pulled out -> back to the desktop */
            if (g_dock_n > DOCK_MIN && g_desk_n < 16) {   /* keep at least DOCK_MIN docked */
                g_drag_item.x = g_drag_x - DESK_CELL_W / 2.0f;   /* land where dropped */
                g_drag_item.y = g_drag_y - DESK_ICON / 2.0f;
                g_drag_item.placed = 1;
                desk_clamp(&g_drag_item);
                g_desk[g_desk_n++] = g_drag_item;  /* return it to the desktop ... */
                for (int k = g_drag_i; k < g_dock_n - 1; k++) g_dock[k] = g_dock[k+1];
                g_dock_n--;                        /* ... and remove from the dock */
                g_dock_dirty = 1;
                desk_save_layout();
            }                                       /* at the minimum -> snaps back */
        } else if (g_dockr[2] > g_dockr[0]) {       /* reorder by x */
            int tgt = (int)(((g_cfg.dock_edge?g_drag_y-g_dockr[1]:g_drag_x-g_dockr[0]) / (g_cfg.dock_edge?g_dockr[3]-g_dockr[1]:g_dockr[2]-g_dockr[0])) * g_dock_n);
            if (tgt < 0) tgt = 0;
            if (tgt >= g_dock_n) tgt = g_dock_n - 1;
            if (tgt != g_drag_i) {
                struct app_item tmp = g_dock[g_drag_i];
                if (tgt > g_drag_i) for (int k=g_drag_i; k<tgt; k++) g_dock[k]=g_dock[k+1];
                else                for (int k=g_drag_i; k>tgt; k--) g_dock[k]=g_dock[k-1];
                g_dock[tgt] = tmp; g_dock_dirty = 1;
            }
        }
    }
    g_drag = 0; g_drag_i = -1; g_drag_moved = 0;
}

/* Desktop icons, each drawn at its OWN position rather than flowing in a list.
 * One out-of-flow overlay spans the desktop and every icon is offset inside it,
 * which is what lets a drop put an icon anywhere instead of returning it to a
 * slot decided by its index. (Same mechanism the drag ghost uses.) */
static void desktop_icons(void) {
    desk_autoplace();
    em_flush();                       /* emit pending DSL leaves before raw ui_* */
    ui_begin_vstack(0xDE5C0DEEULL);
    ui_set_overlay(true);
    /* A ZERO-SIZED root, not a screen-filling one. The icons are positioned by
     * per-child offsets, which need no parent extent -- and a full-screen
     * transparent root CONTAINS every point, so wherever this overlay sits in
     * document order it either eats the dock's clicks (declared after it) or
     * loses the icons' clicks to the work-area strip (declared before it).
     * With no extent of its own, only the ICON BOXES exist for hit testing:
     * each wins exactly its own pixels and nothing else, and declaration order
     * stops mattering at all. That dilemma was this desktop's whole click-bug
     * history in one node. */
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
    ui_set_clip_children(false);
    for (int i = 0; i < g_desk_n; i++) {
        const void *id = g_desk[i].app ? (const void *)g_desk[i].app
                                       : (const void *)g_desk[i].dir;
        ui_begin_vstack(0xDE5C0000ULL ^ (uint64_t)(uintptr_t)id);
        ui_set_offset(g_desk[i].x, g_desk[i].y);
        VStack(.spacing = 5, .width = DESK_CELL_W, .align = Center) {
            drag_icon(g_desk[i], DESK_ICON, 1, i);
            Text(g_desk[i].label).caption().color(g_desk_ink);
        }
        em_flush();
        ui_end_stack();
    }
    ui_end_stack();
}

/* Post-login desktop: wallpaper, three honest folder shortcuts, and a
 * full-width bottom taskbar. The right side deliberately exposes only state the
 * OS can report today; network/audio/battery indicators arrive with their
 * corresponding services instead of being decorative lies. */

/* TAKE A PICTURE OF THE SCREEN.
 *
 * The shell is the right process for this. It is the one thing on the machine
 * that legitimately looks at the whole display, and handing EMBK_CAP_SCREEN to
 * a separate screenshot application would mean a program whose entire purpose
 * is reading everybody else's windows -- sitting in the launcher, one click
 * from anyone.
 *
 * Written as binary PPM because that is what this system's own image loader
 * reads, so a screenshot lands in Pictures and opens in Photos with nothing in
 * between. A format nobody else can open is a file you cannot look at.
 *
 * TWO PHASES, ON TWO THREADS, and that split is the whole design. CAPTURING is
 * fast and must be instantaneous -- it is a photograph, and a photograph taken
 * over several seconds is a photograph of several different screens. WRITING is
 * slow: EMBKFS commits as it goes, and two and a half megabytes takes it the
 * better part of a minute. Doing both here would freeze the desktop for that
 * whole time, which is the same mistake the menu bar made with a blocking
 * channel read, wearing different clothes.
 *
 * So the pixels are grabbed on the render thread and handed to a writer that
 * gets on with it. The desktop keeps drawing; the notification arrives when the
 * file is actually on disk, not when the work was merely started.
 *
 * THE WRITER STILL NEVER TOUCHES MALLOC. It was forced when this was written:
 * newlib's __malloc_lock was a stub that locked nothing, so this writer -- the
 * first thread in the shell ever to allocate -- crashed the desktop in free()
 * with a general protection fault the instant its last write landed, while the
 * render loop was allocating mid-frame. user/lib/syscalls.c provides a real
 * lock now, so this is no longer required; it is kept because one thread
 * owning the buffer's lifetime is simpler to reason about than two, and
 * because changing working code to use a freedom it does not need is not an
 * improvement. */
static uint32_t     *g_shot_px;        /* owned by the writer once handed over */
static int           g_shot_w, g_shot_h;
static char          g_shot_path[192];
static volatile int  g_shot_busy;
static volatile int  g_shot_done;      /* the writer is finished; the buffer is ours again */

static void shot_writer(long arg) {
    (void)arg;
    int w = g_shot_w, h = g_shot_h;
    unsigned char *out = (unsigned char *)g_shot_px;   /* already RGB: see below */

    int fd = (int)embk_open(g_shot_path, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (fd < 0) {
        char b[160];
        snprintf(b, sizeof b, "Could not write %s (%d).", g_shot_path, fd);
        embk_notify_level(EMNOTE_FAIL, "Screenshot", b);
        g_shot_done = 1; g_shot_busy = 0;
        embk_thread_exit(1);
    }
    char hdr[64];
    int hn = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", w, h);
    embk_write(fd, hdr, (size_t)hn);

    /* BIG BLOCKS. The first version wrote one row at a time -- 768 calls for a
     * screen -- and the log showed the object growing in three-kilobyte steps
     * with a filesystem commit behind each one. */
    size_t total = (size_t)w * (size_t)h * 3, off = 0;
    int ok = 1;
    while (off < total) {
        size_t chunk = total - off;
        if (chunk > 256u * 1024u) chunk = 256u * 1024u;
        int64_t wr = embk_write(fd, out + off, chunk);
        if (wr <= 0) { ok = 0; break; }
        off += (size_t)wr;
    }
    embk_close(fd);

    { const char *leaf = g_shot_path;
      for (const char *q = g_shot_path; *q; q++) if (*q == '/') leaf = q + 1;
      char b[200];
      if (ok) snprintf(b, sizeof b, "Saved as Pictures/%s", leaf);
      else    snprintf(b, sizeof b, "%s is incomplete -- the disk refused a write.", leaf);
      embk_notify_level(ok ? EMNOTE_INFO : EMNOTE_FAIL, "Screenshot", b); }
    { char b[220]; snprintf(b, sizeof b, "home: screenshot %s -> %s\n",
                            ok ? "saved" : "FAILED", g_shot_path); embk_puts(1, b); }
    g_shot_done = 1;          /* the render thread owns the buffer again */
    g_shot_busy = 0;
    embk_thread_exit(0);
}

static void take_screenshot(void) {
    if (g_shot_busy) {
        embk_notify_level(EMNOTE_INFO, "Screenshot", "The last one is still saving.");
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        embk_notify_level(EMNOTE_FAIL, "Screenshot",
                          "There is no home directory to save it in.");
        return;
    }
    int w = (int)g_sw, h = (int)g_sh;
    if (w <= 0 || h <= 0) return;

    uint32_t *px = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!px) {
        embk_notify_level(EMNOTE_FAIL, "Screenshot", "Not enough memory to hold it.");
        return;
    }
    int got = embk_screen_read(0, 0, w, h, px, (unsigned)(w * h));
    if (got < w * h) {
        /* SAY WHICH FAILURE. -EPERM means the shell was spawned without
         * EMBK_CAP_SCREEN, which is a configuration answer and not a bug in
         * the picture-taking -- the two want completely different fixes. */
        char b[120];
        if (got == -EMBK_EPERM)
            snprintf(b, sizeof b, "The shell is not allowed to read the screen.");
        else
            snprintf(b, sizeof b, "The screen could not be read (%d).", got);
        embk_notify_level(EMNOTE_FAIL, "Screenshot", b);
        free(px);
        return;
    }

    /* First free number, so a second screenshot never silently replaces the
     * first -- the thing you would least expect and most regret. */
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(g_shot_path, sizeof g_shot_path, "%s/Pictures/Screenshot %d.ppm", home, n);
        int probe = (int)embk_open(g_shot_path, EMBK_O_RDONLY, 0);
        if (probe < 0) break;
        embk_close(probe);
    }

    /* CONVERTED HERE, on the render thread, for the malloc reason above -- and
     * in place, because the output is smaller than the input (three bytes out
     * for every four in), so the write cursor never catches the read cursor
     * going forward and the shell avoids a second whole-screen allocation. */
    { unsigned char *o = (unsigned char *)px;
      for (int i = 0; i < w * h; i++) {
          uint32_t c = px[i];                    /* read before it is overwritten */
          o[i * 3 + 0] = (unsigned char)((c >> 16) & 255);
          o[i * 3 + 1] = (unsigned char)((c >> 8) & 255);
          o[i * 3 + 2] = (unsigned char)(c & 255);
      } }

    g_shot_px = px; g_shot_w = w; g_shot_h = h; g_shot_done = 0; g_shot_busy = 1;
    if (embk_thread_create(shot_writer, 0) < 0) {
        embk_notify_level(EMNOTE_FAIL, "Screenshot", "Could not start the writer.");
        free(px); g_shot_px = 0; g_shot_busy = 0; g_shot_done = 0;
        return;
    }
    embk_notify_level(EMNOTE_INFO, "Screenshot", "Saving to Pictures...");
}

static void home_ui(void) {
    g_any_active = 0; g_have_dockr = 0;   /* recomputed each frame during the build */
    Screen(.width = g_sw, .height = g_sh, .padding = -1, .align = Fill) {
        /* THE DESKTOP PICTURE IS A PREFERENCE, read live like every other one.
         * cfg_poll() re-reads it about once a second, so picking one in
         * Settings changes the screen under you rather than at the next login
         * -- which is the difference between a setting and a promise. */
        {
            int wi = (g_cfg.wallpaper >= 0 && g_cfg.wallpaper < OSCFG_WALLPAPERS)
                     ? g_cfg.wallpaper : 0;
            const struct oscfg_wallpaper *wp = &oscfg_wallpapers[wi];
            if (g_nout > 1) {
                /* SEVERAL MONITORS: each shows the whole picture, not its
                 * slice of one stretched across them all. */
                for (int i = 0; i < g_nout; i++) {
                    at_begin(0xBAC70000ull + (uint64_t)i, g_ox[i], g_oy[i]);
                    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = g_ow[i] },
                                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = g_oh[i] });
                    if (wp->path) {
                        uint32_t iw = 0, ih = 0;
                        const uint32_t *ipx = em_image(wp->path, &iw, &ih);
                        if (ipx) ui_image_fill((uint64_t)(uintptr_t)ipx, ipx, iw, ih);
                    } else {
                        ui_box_begin(0xBAC6D100ull + (uint64_t)i);
                        ui_set_paint(em_lgrad3(
                            (Color){ wp->a[0], wp->a[1], wp->a[2], 1.f },
                            (Color){ wp->b[0], wp->b[1], wp->b[2], 1.f },
                            (Color){ wp->c[0], wp->c[1], wp->c[2], 1.f }, wp->angle));
                        ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = g_ow[i] },
                                    (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = g_oh[i] });
                        ui_box_end();
                    }
                    at_end();
                }
            } else if (wp->path) {
                BackgroundImage(wp->path);
            } else {
                /* A DRAWN wallpaper. Same node, same place in the tree as the
                 * image one -- the desktop does not gain a mode, the ground
                 * just has a different paint. */
                em_flush();
                ui_box_begin(0xBAC6D000ULL);
                ui_set_paint(em_lgrad3(
                    (Color){ wp->a[0], wp->a[1], wp->a[2], 1.f },
                    (Color){ wp->b[0], wp->b[1], wp->b[2], 1.f },
                    (Color){ wp->c[0], wp->c[1], wp->c[2], 1.f }, wp->angle));
                ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = g_sw },
                            (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = g_sh });
                ui_box_end();
            }
        }
        {
            float px,py;ui_pointer_pos(&px,&py);
            int band=oscfg_dock_band(&g_cfg);
            /* On the primary monitor, like the top bar. */
            int on_primary=px>=g_px && px<g_px+g_pw && py>=g_py && py<g_py+g_ph;
            int show=!g_cfg.dock_autohide || g_drag || (on_primary &&
                (g_cfg.dock_edge==1?px<g_px+band:g_cfg.dock_edge==2?px>g_px+g_pw-band:py>g_py+g_ph-band-(g_cfg.bar_edge?g_cfg.bar_size:0)));
            float extent=24+g_dock_n*DOCK_SLOT_W+(g_dock_n?g_dock_n-1:0)*10;
            float x=g_px+(g_cfg.dock_edge==1?10:g_cfg.dock_edge==2?g_pw-DOCK_PILL_H-10:(g_pw-extent)*.5f);
            float y=g_py+(g_cfg.dock_edge?(g_ph-extent)*.5f:g_ph-DOCK_BAND-(g_cfg.bar_edge?g_cfg.bar_size:0));
            em_flush();ui_begin_vstack(0xD0C4B0);ui_set_overlay(true);
            ui_set_size((struct layout_size){.mode=SIZE_FIXED,.fixed_value=0},(struct layout_size){.mode=SIZE_FIXED,.fixed_value=0});
            ui_set_clip_children(false);
            ui_begin_vstack(1);ui_set_offset(x,y);
            if(show)dock_pill();
            else HStack(.width=g_cfg.dock_edge?3:72,.height=g_cfg.dock_edge?72:3,.background=T.border_strong,.corner=2) {}
            ui_end_stack();ui_end_stack();
        }
        if(g_cfg.desktop_icons)desktop_icons();
       /* freely-placed icons; zero-sized root, so it
                                * can sit last without shadowing anything */

        /* Right-click the desktop. Every item does something REAL and
         * immediate -- no greyed-out placeholders, which are just an apology
         * in menu form. "Clean Up" exists because free placement earned it:
         * once icons can go anywhere, you need one gesture to make them tidy
         * again. */
        {
            float rx, ry;
            if (!g_locked && RightClicked(&rx, &ry)) {
                g_ctx_x = rx; g_ctx_y = ry; g_ctx_open = true;
                g_dock_dirty = 1;
            }
        }
        ContextMenu(&g_ctx_open, g_ctx_x, g_ctx_y) {
            if (MenuItem("New Terminal")) g_launch = "/data/apps/term/term.elf";
            if (MenuItem("Open Files"))   launch_folder(getenv("HOME") ? getenv("HOME") : "/");
            MenuSeparator();
            if (MenuItem("Show Applications")) apps_set_open(1);
            if(MenuItem("Desktop Settings"))g_launch="/data/apps/settings/settings.elf";
            MenuSeparator();
            if (MenuItem("Take Screenshot")) g_want_shot = 1;
            if (MenuItem("Clean Up Icons")) {
                /* forget every position; autoplace re-columns them next frame */
                for (int i = 0; i < g_desk_n; i++) g_desk[i].placed = 0;
                desk_save_layout();
                g_dock_dirty = 1;
            }
        }
        apps_grid();           /* the Apps launcher (modal grid), when open */
        lock_ui();             /* ...and the screen lock, over even that */
        if(!g_locked && !g_apps_open)dock_label();          /* names the dock icon under the pointer (overlay) */
        drag_ghost();          /* the dragged icon follows the cursor (overlay) */
    }
    dock_resolve_drop();       /* act on a released drag (add / remove / reorder) */
}

/* One instance per app: remember each child's spawn HANDLE (what embk_spawn
 * returns -- NOT a pid; handles are 0-based, stored here +1 so the zeroed
 * static table means "none") and refuse to spawn again while that child is
 * alive. Once it has exited or been closed, embk_wait() the dead child BEFORE
 * respawning: that reaps its zombie process slot AND frees the spawn handle.
 * Without the wait, every launch leaked one of the 16 per-process handles and
 * the 17th spawn failed -- killing its own child on the spot; and the old code
 * stored the handle AS a pid, so proc_alive() interrogated some unrelated
 * always-alive low pid (the shell/idle) and refused every relaunch. */
#define MAX_TRACKED 8
static struct {
    char path[96];          /* exec path, COPIED -> a stable identity even when the
                             * caller's pointer is a reused scan buffer (grid apps) */
    char start_dir[256];    /* "" = none; distinguishes independent Files shortcuts */
    int  used;
    int  handle_p1;
} g_running[MAX_TRACKED];

/* WHAT IS ACTUALLY ON SCREEN, as opposed to what this process remembers
 * starting. Sampled on a clock rather than per frame: the dock repaints
 * whenever the pointer moves over it, and asking the kernel to walk its window
 * table for every one of those would be a syscall per mouse event to answer a
 * question whose answer changes when an app opens or closes. Half a second is
 * faster than a person can notice and thousands of times cheaper. */
#define WINS_MAX 32
static struct embk_win_info g_wins[WINS_MAX];
static int      g_wins_n;
static uint64_t g_wins_next;

static void wins_poll(void) {
    uint64_t now = embk_uptime_ms();
    if (now < g_wins_next) return;
    g_wins_next = now + 500;
    int n = embk_win_list(g_wins, WINS_MAX);
    g_wins_n = n > 0 ? n : 0;
}

/* The pid of a live window belonging to this exec path, or 0.
 *
 * MATCHED ON THE BINARY'S BASENAME, never on the window title: an app rewrites
 * its title as soon as you open something in it, so a dock that recognised
 * apps by title would light a tile until the moment the app was used. The
 * kernel records the basename at spawn and never lets anything decide with it
 * -- see struct process::exec_name. */
static uint32_t win_pid_for(const char *path) {
    if (!path || !path[0]) return 0;
    const char *base = embk_basename(path);
    for (int i = 0; i < g_wins_n; i++)
        if (g_wins[i].app[0] && !strcmp(g_wins[i].app, base)) return g_wins[i].pid;
    return 0;
}

/* Is the app behind this dock chip alive right now? Drives the dock's lit
 * socket, so a lit tile is a RUNNING process, not a memory of one.
 *
 * TWO SOURCES, because neither alone is the truth:
 *
 *   * our own spawn handles answer for the instant between clicking a tile and
 *     the app putting a window up -- there is nothing on screen yet, and a
 *     socket that stays dark for that half-second reads as a click that did
 *     not work;
 *   * the window list answers for everything else in the session, which is
 *     what this could not do before. An app started from the shell, or by
 *     another app, or one the desktop launched and then lost the handle to,
 *     left its tile dark while its window sat in plain view on screen.
 *
 * The dock's job is to say what is running on the machine, not what this
 * particular process happens to have started. */
static int dock_running(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < MAX_TRACKED; i++)
        if (g_running[i].used && g_running[i].handle_p1 > 0 &&
            strcmp(g_running[i].path, path) == 0 &&
            embk_proc_alive(g_running[i].handle_p1 - 1) == 1)
            return 1;
    return win_pid_for(path) != 0;
}

/* An app declares the authority it needs in two sidecars beside its binary --
 * <name>.ns (what it may NAME) and <name>.caps (what it may DO). The parsing
 * lives in user/lib/appauth.c because reading an app's declaration is not the
 * desktop's business specifically; any launcher asks the same question.
 * See docs/USERSPACE_v2.md UP4. */
#define NS_ACTS_MAX 8

static void spawn_app(const char *path, const char *start_dir) {
    const char *sd = start_dir ? start_dir : "";
    /* Dedup by exec+dir CONTENT, not pointer: the same app reached from the dock
     * (a string literal) and from the launcher grid (a reused scan buffer) has
     * two different addresses -- pointer-equality let it spawn TWICE, and two
     * instances fighting over one window rendered as a dead black rectangle. */
    int slot = -1, freeslot = -1, deadslot = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_running[i].used) {
            if (!strcmp(g_running[i].path, path) &&
                !strcmp(g_running[i].start_dir, sd)) { slot = i; break; }
            /* A slot whose app has EXITED is reusable. `used` was never
             * cleared once set, so after eight distinct apps this table was
             * permanently full and every further launch returned silently --
             * apps simply stopped starting, with no dot and no error. The
             * table is a cache of what is running, not a registry of what has
             * ever run. */
            if (deadslot < 0 && (g_running[i].handle_p1 <= 0 ||
                                 !embk_proc_alive(g_running[i].handle_p1 - 1)))
                deadslot = i;
        } else if (freeslot < 0) freeslot = i;
    }

    if (slot >= 0) {                       /* already tracked */
        if (g_running[slot].handle_p1 > 0) {
            int h = g_running[slot].handle_p1 - 1;
            if (embk_proc_alive(h)) {
                /* Still running -- one instance only. But "the app is already
                 * open" is not a reason to do NOTHING: that is what made a dock
                 * click on a running app feel broken, and it is the only way
                 * back for a window the user minimized. Bring it forward. */
                embk_win_restore(h);
                return;
            }
            embk_wait(h);                     /* dead: reap the zombie + free the handle */
            g_running[slot].handle_p1 = 0;
        }
    } else {
        slot = freeslot >= 0 ? freeslot : deadslot;   /* evict an exited app */
        if (slot < 0) return;              /* genuinely full: all still running */
        if (g_running[slot].used && g_running[slot].handle_p1 > 0)
            embk_wait(g_running[slot].handle_p1 - 1);   /* reap before reusing */
        g_running[slot].handle_p1 = 0;

        /* NOT OURS, BUT ALREADY RUNNING -- the other half of the dock telling
         * the truth. The tile is lit because the window list says this program
         * has a window; clicking it must then go to that window, because
         * spawning a second copy of an app whose tile you just saw lit is the
         * worst of both answers: the click appears to do nothing (the new
         * instance opens behind, or fights the first one for its window) and
         * the machine is now running two.
         *
         * A ZERO RETURN MEANS "nothing to raise" -- the sample is up to half a
         * second old, so the app may have exited since -- and then we fall
         * through and start it, which is what the click asked for.
         *
         * Only for a plain launch. A shortcut that carries a START DIRECTORY
         * (Files opened at /data while Files is already open at /home) is
         * deliberately a SEPARATE instance, and raising the existing one would
         * silently refuse to open the place the user pointed at. */
        if (!sd[0]) {
            uint32_t rpid = win_pid_for(path);
            if (rpid && embk_win_raise(rpid) > 0) return;
        }
    }

    /* Grant the child EXACTLY its declared namespace (UP4); no manifest => it
     * inherits our full view. */
    struct embk_spawn_file_action acts[NS_ACTS_MAX + 1];
    char nsdesc[224], capdesc[128];
    int nacts = appauth_load_ns(path, acts, NS_ACTS_MAX, nsdesc, sizeof nsdesc);

    /* ...and the capability half of the same declaration. A SET_CAPS action
     * goes FIRST so the log reads in the order authority is decided, though the
     * kernel applies each action independently. An app with no .caps inherits
     * ours, exactly as an app with no .ns inherits our namespace -- silence is
     * the pre-manifest behaviour, and only a manifest can narrow. */
    unsigned capmask = 0;
    int have_caps = appauth_load_caps(path, &capmask, capdesc, sizeof capdesc);
    if (have_caps && nacts < NS_ACTS_MAX + 1) {
        /* shift the ns binds along to keep SET_CAPS first */
        for (int k = nacts; k > 0; k--) acts[k] = acts[k - 1];
        embk_action_set_caps(&acts[0], capmask);
        nacts++;
    }

    char *argv[] = { (char *)path, NULL };
    char file_path_env[640];
    char tz_env[72];
    char *launch_env[64];

    /* THE ZONE TRAVELS WITH THE CHILD. Nothing is inherited in this system --
     * the kernel gives a child no environment unless the parent names one (see
     * the kernel's spawn.h) -- so an application that formats a date gets the
     * user's zone only because the desktop hands it over here.
     *
     * This is the one place it can happen. An app that reads the preferences
     * wears the zone itself, but most do not read them: the file manager
     * showing modification times, a text editor, anything run from the
     * terminal. They all come through this function.
     *
     * A zone already in the session environment is dropped rather than
     * duplicated -- the live preference is the newer answer. */
    int en = 0;
    for (int k = 0; g_session_env && g_session_env[k] && en < 61; k++) {
        if (!strncmp(g_session_env[k], "TZ=", 3)) continue;
        launch_env[en++] = g_session_env[k];
    }
    snprintf(tz_env, sizeof tz_env, "TZ=%s", oscfg_tz_string(&g_cfg));
    launch_env[en++] = tz_env;
    if (start_dir && start_dir[0] == '/') {
        snprintf(file_path_env, sizeof file_path_env, "FILES_PATH=%s", start_dir);
        launch_env[en++] = file_path_env;
    }
    launch_env[en] = NULL;
    char **env = launch_env;
    int h = (int)embk_spawn_env(path, argv, env, nacts ? acts : NULL, nacts);

    char b[448];
    int binds = nacts - (have_caps ? 1 : 0);
    if (binds || have_caps)
        snprintf(b, sizeof b, "home: spawn %s -> ns[%s] caps[%s]\n", path,
                 binds ? nsdesc : "inherit", have_caps ? capdesc : "inherit");
    else
        snprintf(b, sizeof b, "home: spawn %s -> full inherit (no manifest)\n", path);
    embk_puts(1, b);

    if (h >= 0) {
        snprintf(g_running[slot].path, sizeof g_running[slot].path, "%s", path);
        snprintf(g_running[slot].start_dir, sizeof g_running[slot].start_dir, "%s", sd);
        g_running[slot].used = 1;
        g_running[slot].handle_p1 = h + 1;
    }
    else {
        char e[96]; snprintf(e, sizeof e, "home: spawn %s FAILED: %d\n", path, h);
        embk_puts(1, e);
        /* AND SAY IT WHERE SOMEBODY CAN SEE IT. This went to fd 1 and nowhere
         * else: on the machine this OS is meant to run on, that is a serial
         * cable nobody has plugged in, so clicking an app that cannot start
         * looked exactly like clicking an app that does nothing. The failure a
         * user is most likely to meet was the one the system was quietest
         * about. */
        const char *leaf = path;
        for (const char *q = path; *q; q++) if (*q == '/') leaf = q + 1;
        char body[160];
        snprintf(body, sizeof body, "%s could not be started (error %d).", leaf, h);
        embk_notify_level(EMNOTE_FAIL, "Application did not start", body);
    }
}

/* DROP DOCK ENTRIES WHOSE PROGRAM IS NOT ON THIS IMAGE.
 *
 * The dock's starting line-up is a table in this file; what is actually
 * installed is a property of the build. NetSurf is the case that made this
 * necessary -- it is a port built from a tree that is not always present, so
 * the image regularly ships without nsemblink.elf while the dock kept offering
 * it. Clicking it did nothing at all, which is the exact failure this shell
 * spends so much effort avoiding everywhere else: the Applications launcher
 * scans the disk and correctly never showed it, and the dock, which does not,
 * showed it every boot.
 *
 * A dock that offers what cannot be launched is a dock that lies, and a user
 * has no way to tell that from a broken click. So ask the filesystem once, at
 * start, and only offer what is there. Items the user dragged in themselves
 * came from something that existed; this only prunes the built-in defaults. */
static void dock_prune_missing(void) {
    int out = 0;
    for (int i = 0; i < g_dock_n; i++) {
        const char *exe = g_dock[i].app;
        if (exe) {
            int fd = (int)embk_open(exe, EMBK_O_RDONLY, 0);
            if (fd < 0) {
                char b[128];
                snprintf(b, sizeof b, "home: dock: %s is not installed -- not offering it\n", exe);
                embk_puts(1, b);
                continue;
            }
            embk_close(fd);
        }
        if (out != i) g_dock[out] = g_dock[i];
        out++;
    }
    g_dock_n = out;
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    g_session_env = envp;oscfg_defaults(&g_cfg_published);
    dock_prune_missing();                   /* offer only what can actually run */
    embk_thread_create(apps_listener, 0);   /* the top bar's Apps signal listener */
    embk_thread_create(prefs_listener, 0);  /* ...and what it wears while it draws */
    embk_session(EMBK_SESSION_BEGIN,0);
    embk_thread_create(lock_listener, 0);   /* ...and the menu bar's Lock Screen */
    /* apps describe their own icon/name (docs presentation manifest) */
    load_app_meta("files", g_dock[0].icon, sizeof g_dock[0].icon, g_dock[0].label, sizeof g_dock[0].label);
    load_app_meta("term",  g_dock[1].icon, sizeof g_dock[1].icon, g_dock[1].label, sizeof g_dock[1].label);
    /* toolkit font + context */
    size_t rl = 0;
    uint8_t *reg = read_file("/system/fonts/font.ttf", &rl);
    uint32_t fr = reg ? font_load(reg, rl) : 0;
    if (fr) font_install_backend();
    embk_puts(1, fr ? "home: font loaded\n" : "home: FONT MISSING\n");

    struct scene_arena sa; scene_arena_init(&sa);
    struct layout_arena la; layout_arena_init(&la);
    ui_theme_set_fonts(fr, fr);
    ui_theme_use_dark(true);
    ui_init(&sa, &la);
    em_res_set_loader(read_file);

    /* Take the full screen as the compositor's desktop layer (zero-copy): the
     * toolkit renders its launcher straight into the shared pages. */
    void *pixels = 0; uint32_t sw = 0, sh = 0;
    int win = embk_win_create_desktop(&pixels, &sw, &sh);
    if (win < 0 || !pixels || sw == 0 || sh == 0) {
        embk_puts(1, "home: desktop create FAILED\n");
        return 1;
    }
    g_sw = (float)sw; g_sh = (float)sh;   /* home_ui sizes the Screen to these */
    outputs_poll();
    /* ...and TELL THE TOOLKIT, which em_app_run would have done for us. em
     * sizes popover dismiss-scrims to the viewport, so an unset one leaves a
     * 0x0 scrim: the context menu would open and then refuse to be dismissed
     * by clicking away from it. */
    em_set_viewport(g_sw, g_sh);
    /* Restore where the user last left each icon. Must run AFTER the screen
     * size is known: the loader clamps every position into the work area, and
     * clamping against a 0x0 screen would collapse the whole desktop into one
     * corner. */
    desk_load_layout();

    struct render_target rt = { (uint32_t *)pixels, sw, sh, sw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());

    em_set_clock(embk_uptime_ms);
    /* Our menu bar IS topbar.elf -- the Apple-modern glass bar with the drag
     * handle, draggable dock chips, and pin/snap. The desktop no longer draws
     * its own top status bar; this floating bar takes the top strip. */
    spawn_app("/data/apps/topbar/topbar.elf", NULL);
    /* THE SESSION'S SERVICES. The file panel is not an application -- nothing
     * launches it from the dock -- it is the program that answers
     * /run/emlink.files when any app asks the user for a file. Started with
     * the session because it must be there BEFORE the first app needs it: a
     * service you have to launch by hand is a service every caller has to
     * handle the absence of. */
    spawn_app("/system/bin/filepanel.elf", NULL);
    spawn_app("/system/bin/notifyd.elf", NULL);
    embk_puts(1, "home: desktop ready\n");

    uint64_t screen_next=0,restore_token=0,arr_token=0;int restore_attempted=0,restore_presented=0;
    uint32_t arr_set_tried=0,presented_gen=0;   /* the display layout the last presented frame was drawn for */
    for (;;) {
        cfg_poll(); /* Load saved preferences before deciding whether to restore a mode. */
        if(embk_uptime_ms()>=screen_next) {
            screen_next=embk_uptime_ms()+200;uint32_t nw=0,nh=0;embk_screen_size(&nw,&nh);
            uint32_t gen_before=g_out_gen;outputs_poll();
            if(g_out_gen!=gen_before && nw==sw && nh==sh) {
                /* The monitors moved under the same desktop size: re-place, repaint. */
                for(int i=0;i<g_desk_n;i++)desk_clamp(&g_desk[i]);
                g_invalidate=2;g_full_present=2;em_structure_changed();
            }
            if(nw && nh && (nw!=sw || nh!=sh)) {
                void *next=NULL;
                if(embk_win_resize(win,nw,nh,&next)>=0 && next) {
                    restore_presented=0;pixels=next;sw=nw;sh=nh;g_sw=nw;g_sh=nh;em_set_viewport(nw,nh);
                    outputs_poll();
                    rt=(struct render_target){(uint32_t *)pixels,nw,nh,nw*4,EMBK_PIXFMT_BGRA8888_PRE};
                    scene_render_destroy(&r);scene_render_init(&r,cpu_backend_get());
                    for(int i=0;i<g_desk_n;i++)desk_clamp(&g_desk[i]);
                    g_invalidate=2;g_full_present=2;em_structure_changed();
                }
            }
        }
        if(!restore_attempted || restore_token) {
            struct embk_monitor_info monitor;
            if(!embk_monitor(EMBK_MONITOR_GET,&monitor,sizeof monitor)) {
                if(restore_token) {
                    if(monitor.token!=restore_token || !(monitor.flags&EMBK_MONITOR_OWN_TRIAL))restore_token=0;
                    else if(restore_presented && monitor.phase==EMBK_MONITOR_TRIAL && sw==monitor.width && sh==monitor.height) {
                        struct embk_monitor_request req={.version=1,.token=restore_token};
                        if(!embk_monitor(EMBK_MONITOR_KEEP,&req,sizeof req))restore_token=0;
                    }
                }else if(g_cfg.display_width && g_cfg.display_height && (monitor.flags&EMBK_MONITOR_MODESET) && g_out_connected<2) {
                    /* One monitor's resolution. With several connected, what
                     * comes back is their arrangement, below. */
                    if(monitor.width==(unsigned)g_cfg.display_width && monitor.height==(unsigned)g_cfg.display_height)restore_attempted=1;
                    else if(monitor.phase==EMBK_MONITOR_IDLE || monitor.phase==EMBK_MONITOR_ERROR) {
                        struct embk_monitor_request req={1,monitor.generation,g_cfg.display_width,g_cfg.display_height,0};
                        int rc=embk_monitor(EMBK_MONITOR_APPLY,&req,sizeof req);
                        if(!rc) {restore_attempted=1;embk_monitor(EMBK_MONITOR_GET,&monitor,sizeof monitor);restore_token=monitor.token;}
                        else if(rc!=-1)restore_attempted=1;
                    }
                }else restore_attempted=1;
            }
        }
        /* SEVERAL MONITORS: the arrangement kept in Settings comes back
         * whenever exactly its monitors are connected -- at login, and every
         * time they are plugged in again, by which point the kernel has put
         * the newcomer on the right. The same trial as the resolution above,
         * confirmed once the desktop has drawn a frame at the arranged size.
         * An arrangement made for other monitors is left alone
         * (display_arrange.h), and nothing is tried behind the lock. */
        if(arr_token || (g_out_set!=arr_set_tried && !g_locked)) {
            struct embk_display_layout d;
            if(!embk_display_layout(&d)) {
                if(arr_token) {
                    struct embk_monitor_info m;
                    if(embk_monitor(EMBK_MONITOR_GET,&m,sizeof m) || m.token!=arr_token || !(m.flags&EMBK_MONITOR_OWN_TRIAL))arr_token=0;
                    else if(restore_presented && presented_gen==d.generation && m.phase==EMBK_MONITOR_TRIAL &&
                            sw==d.desktop_width && sh==d.desktop_height) {
                        struct embk_monitor_request req={.version=1,.token=arr_token};
                        if(!embk_monitor(EMBK_MONITOR_KEEP,&req,sizeof req)) {
                            arr_token=0;embk_puts(1,"home: saved display arrangement restored\n");
                        }
                    }
                }else if(d.phase==EMBK_MONITOR_IDLE || d.phase==EMBK_MONITOR_ERROR) {
                    uint32_t set=arrange_connected_set(&d);
                    struct embk_layout_request r;
                    if(!(d.flags&EMBK_LAYOUT_MULTI) || arrange_connected(&d)<2 ||
                       arrange_saved_request(&d,&g_cfg.display_layout,&r)!=1)arr_set_tried=set;
                    else {
                        int rc=embk_monitor(EMBK_MONITOR_LAYOUT_APPLY,&r,sizeof r);
                        if(!rc) {
                            struct embk_monitor_info m;
                            if(!embk_monitor(EMBK_MONITOR_GET,&m,sizeof m))arr_token=m.token;
                            restore_presented=0;arr_set_tried=set;
                        }else if(rc!=-EMBK_EPERM && rc!=-EMBK_EBUSY && rc!=-EMBK_EAGAIN)arr_set_tried=set;
                    }
                }
            }
        }
        /* TAKEN HERE, a frame after the menu item was chosen and a frame after
         * the menu closed -- a screenshot of the menu you used to ask for a
         * screenshot is a screenshot of the wrong thing. */
        if (g_locked) g_want_shot = 0;      /* not of a locked screen, and not to
                                             * the locked-out user's Pictures */
        if (g_want_shot && ++g_shot_delay > 2) { g_want_shot = 0; g_shot_delay = 0;
                                                 take_screenshot(); }
        /* The writer has finished with the buffer, so the thread that allocated
         * it is the one that gives it back. */
        if (g_shot_done) { g_shot_done = 0; free(g_shot_px); g_shot_px = 0; }
        desk_ink_poll();       /* labels legible on whatever picture is behind them */
        wins_poll();           /* what is actually running, ours or not */
        poll_apps_request();   /* the top bar's Apps button opens our launcher */
        poll_lock_request();   /* ...and its Lock Screen puts the lock up */

        /* Esc closes the launcher. Only worth reading while it is up: the
         * desktop layer is in FRONT then, so the compositor gives it the
         * keyboard, and draining keys at any other time would take them from
         * whichever app the user is actually typing into. */
        /* LOCKED FIRST, and Esc is not a way out. Every key goes to the
         * password field -- there is nothing else on the screen to type into,
         * and nothing behind it may have them either. */
        if (g_locked) {
            for (int input; (input=embk_key_poll_input())>0; )
                if(!g_lock_checking)ui_input_key(input&255,(unsigned)(input>>8)&255);
        } else if (g_apps_open) {
            for (int input; (input=embk_key_poll_input())>0; ) {
                int c=input&255;
                if (c == 27) { apps_set_open(0); break; }   /* Esc dismisses */
                /* Everything else goes to the search field. The desktop has no
                 * other text to type into, so there is nothing to arbitrate
                 * between -- and typing straight into the search box, without
                 * clicking it first, is most of why searching a launcher is
                 * faster than looking through it. */
                ui_input_key(c,(unsigned)(input>>8)&255);
            }
        } else {
            /* The kernel focus gate permits only desktop-owned input here.
             * Discard idle desktop typing rather than replaying it into search. */
            while(embk_key_poll()>0){}
        }

        /* pointer: the compositor routes the desktop's content-local mouse to us */
        struct embk_win_input in;
        embk_win_input(&in);
        if(in.win==EMBK_WIN_ACTION_LAUNCHER && !g_locked)apps_set_open(!g_apps_open);
        /* the SAME feed em_app_run uses -- home has its own loop (it owns the
         * back-pinned desktop layer, which the app runtime does not create),
         * but it must never have its own idea of what the toolkit needs. */
        ui_pointer_at_time(in.when);
        em_feed_pointer((float)in.x, (float)in.y,
                        in.buttons & EMBK_MOUSE_LEFT, in.buttons & EMBK_MOUSE_RIGHT,
                        in.wheel, in.focused);

        /* live uptime clock in the header */
        uint64_t secs = embk_uptime_ms() / 1000;
        snprintf(g_clock, sizeof g_clock, "up %lu:%02lu",
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm) strftime(g_datetime, sizeof g_datetime, "%a %d %b  %H:%M", tm);

        g_launch = 0;
        g_launch_dir = 0;
        ui_frame_begin(); em_new_frame(); home_ui(); em_flush();em_tooltip_host();ui_frame_end();
        ui_run_layout((float)sw, (float)sh);

        /* Structural transitions invalidate a bounded number of frames above.
         * Ordinary dock changes and drags keep partial repaints. This loop used to nuke the renderer's rect
         * cache (two whole frames' worth) on every dock change and all through
         * a drag, because the dirty tracker lost the pixels of MOVED and
         * DESTROYED nodes -- the closed launcher stayed painted, dragged icons
         * left trails. That hole is fixed where it belonged, in scene_render
         * (moved nodes union old+new footprints; vacated slots -- Step 1b --
         * surface their last footprint from the renderer's cache), so partial
         * repaints are simply always correct now, and cost what changed. */
        g_dock_dirty = 0;

        if (g_invalidate > 0) { g_invalidate--; scene_render_invalidate(&r); }
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        /* Nothing dirty is nothing to present. This used to present the whole
         * desktop instead -- a full-screen compose on every frame the desktop
         * built and changed nothing in, which while idle was ten a second
         * (docs/DESKTOP_PERFORMANCE.md). g_full_present still pushes the
         * whole surface after a structural change. */
        if (r.full || g_full_present > 0) {
            if (g_full_present > 0) g_full_present--;
            if(embk_win_present(win, pixels, sw, sh)>=0){restore_presented=1;presented_gen=g_out_gen;}
        } else if (r.n_dirty > 0 || r.has_scroll_present) {
            int x0 = 1 << 29, y0 = 1 << 29, x1 = -(1 << 29), y1 = -(1 << 29);
            if (r.has_scroll_present) {           /* a scroll blit moved pixels */
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
            if (x1 > (int)sw) x1 = (int)sw;
            if (y1 > (int)sh) y1 = (int)sh;
            if (x1 > x0 && y1 > y0)
                embk_win_present_rect(win, pixels, sw, sh, x0, y0, x1 - x0, y1 - y0);
        }

        /* a tile was clicked this frame -> launch it as a floating window.
         * Never while locked: the surface is over the dock, but a stray click
         * that reached one anyway must not start a program on a machine whose
         * owner has walked away. */
        if (g_launch && !g_locked) spawn_app(g_launch, g_launch_dir);

        /* THE NEXT PASS. Something moving -- a frame asked for (a tooltip, a
         * toast, a caret), an icon being dragged, the surface still to be
         * pushed whole -- waits for the compositor's next frame. Nothing
         * moving waits for input, or for the next of this loop's own polls:
         * the display check every 200 ms is the soonest. This loop rebuilt
         * and laid out the desktop every 15 ms whatever happened; still, it
         * now wakes five times a second. */
        if (em_take_frame_request() || g_drag || g_invalidate > 0 || g_full_present > 0)
            em_wait_frame(15);
        else {
            uint64_t now = embk_uptime_ms();
            em_wait_input(screen_next > now ? (int)(screen_next - now) : 0);
        }
    }
    return 0;
}
