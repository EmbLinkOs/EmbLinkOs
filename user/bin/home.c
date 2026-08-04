/* user/bin/home.c -- the EmbLink OS HOME launcher.
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
static char      **g_session_env = 0;

static void launch_folder(const char *path) {
    g_launch = "/data/apps/files/files.elf";
    g_launch_dir = path;
}

/* An app the user can click (launch/open) OR drag. `app` spawns an elf; `dir`
 * opens Files at a folder (one of the two is set). icon/label are BUFFERS so an
 * app can supply them from its own <name>.app presentation manifest; the stable
 * IDENTITY (for reconciler keys) is the app/dir path, never the icon buffer. */
struct app_item { char icon[96]; char label[24]; const char *app; const char *dir; };

/* Apps live EITHER on the desktop OR in the dock; dragging MOVES one between the
 * two (never a copy), so a name never lingers behind. Home's folder is $HOME,
 * filled in once at startup. */
static struct app_item g_desk[16] = {
    { "/system/images/icon-launcher.pam", "Apps",   0, "/data/apps" },
    { "/system/images/icon-settings.pam", "System", 0, "/system" },
};
static int g_desk_n = 2;

/* the bottom app dock -- a centered pill sized to its apps. Grows as apps are
 * dragged in / shrinks as they are dragged out, but never below DOCK_MIN. */
#define DOCK_MIN 2
static struct app_item g_dock[16] = {
    { "/system/images/icon-files.pam",    "Files",    "/data/apps/files/files.elf", 0 },
    { "/system/images/icon-terminal.pam", "Terminal", "/data/apps/term/term.elf",   0 },
};
static int g_dock_n = 2;

/* Fill an item's icon+label from the app's OWN presentation manifest at
 * /data/apps/<name>/<name>.app ("name X" / "icon Y" lines) -- so the desktop
 * reflects what the app declares, not a hard-coded table. Missing manifest or
 * field => the pre-seeded fallback stays. */
static void load_app_meta(const char *name, struct app_item *it) {
    char path[128];
    snprintf(path, sizeof path, "/data/apps/%s/%s.app", name, name);
    size_t len = 0;
    uint8_t *buf = read_file(path, &len);
    if (!buf) return;
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
        if (kl==4 && !memcmp(buf+ks,"name",4)) { dst = it->label; cap = sizeof it->label; }
        else if (kl==4 && !memcmp(buf+ks,"icon",4)) { dst = it->icon; cap = sizeof it->icon; }
        if (dst && cap) { size_t n = vl < cap-1 ? vl : cap-1; memcpy(dst, buf+vs, n); dst[n] = 0; }
    }
    free(buf);
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
static int   g_dock_dirty = 0;       /* dock changed -> the loop force-repaints (no ghosts) */

static void open_item(struct app_item it) {
    if (it.dir)      launch_folder(it.dir);
    else if (it.app) g_launch = it.app;
}

/* One draggable icon. kind: 1 = desktop source, 2 = dock item. Renders the icon
 * (with hover styling) and tracks a press as either a click or a drag; the drop
 * is resolved after the frame in dock_resolve_drop(). */
static void drag_icon(struct app_item it, int size, int kind, int idx) {
    /* Key by the app's IDENTITY (its unique path), NOT its slot index. When the
     * list shifts (remove/reorder), an index-keyed instance gets reused for a
     * different app -- which left a removed middle app lingering (dimmed) in
     * place while the pill kept its old width. Identity keys track each app. */
    uint64_t key = (kind == 1 ? 0xDE510000ULL : 0xD0C00000ULL)
                 ^ (uint64_t)(uintptr_t)(it.app ? it.app : it.dir);
    ui_box_begin(key);
    (void)ui_open();
    ui_set_align(ALIGN_CENTER);
    /* ALWAYS set opacity, so an instance never stays dimmed after its drag ends */
    ui_set_opacity((g_drag == kind && g_drag_i == idx && g_drag_moved) ? 0.3f : 1.0f);
    ImageButton(it.icon, size);         /* styling only; click/drag handled below */
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
            if (dx*dx + dy*dy > 49.0f) g_drag_moved = 1;   /* > ~7px = a drag */
        }
    }
    ui_box_end();
}

/* The dock: a centered pill sized to its apps. Captures its own world rect so
 * the drop can hit-test against it. */
static void dock_pill(void) {
    HStack(.height = 52, .spacing = 10, .px = 10, .align = Center,
           .background = { .r=.03f, .g=.033f, .b=.045f, .a=.96f },
           .corner = 18, .border = 1, .shadow = 2) {
        (void)ui_open();
        { float x, y, w, h; g_have_dockr = ui_open_rect(&x, &y, &w, &h);
          if (g_have_dockr) { g_dockr[0]=x; g_dockr[1]=y; g_dockr[2]=x+w; g_dockr[3]=y+h; } }
        for (int i = 0; i < g_dock_n; i++)
            drag_icon(g_dock[i], 40, 2, i);
        if (g_dock_n == 0) { EmProps hp = {0}; (void)hp; Text("  drag apps here  ").caption().secondary(); }
    }
}

/* The ghost: the dragged icon following the cursor (an out-of-flow overlay). */
static void drag_ghost(void) {
    if (!(g_drag && g_drag_moved)) return;
    ui_begin_vstack(0x6057);
    ui_set_overlay(true);            /* fill parent, out of flow */
    ui_begin_vstack(1);
    ui_set_offset(g_drag_x - 22.0f, g_drag_y - 22.0f);
    ui_set_opacity(0.85f);
    ImageButton(g_drag_item.icon, 44);
    ui_end_stack();
    ui_end_stack();
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
    } else if (g_drag == 1) {                      /* desktop icon dragged onto the dock */
        if (over && g_dock_n < 16) {
            g_dock[g_dock_n++] = g_drag_item;      /* add to dock ... */
            for (int k = g_drag_i; k < g_desk_n - 1; k++) g_desk[k] = g_desk[k+1];
            g_desk_n--;                            /* ... and REMOVE from the desktop */
            g_dock_dirty = 1;
        }                                          /* dropped off the dock -> snaps back */
    } else if (g_drag == 2) {                       /* dock item */
        if (!over) {                                /* pulled out -> back to the desktop */
            if (g_dock_n > DOCK_MIN && g_desk_n < 16) {   /* keep at least DOCK_MIN docked */
                g_desk[g_desk_n++] = g_drag_item;  /* return it to the desktop ... */
                for (int k = g_drag_i; k < g_dock_n - 1; k++) g_dock[k] = g_dock[k+1];
                g_dock_n--;                        /* ... and remove from the dock */
                g_dock_dirty = 1;
            }                                       /* at the minimum -> snaps back */
        } else if (g_dockr[2] > g_dockr[0]) {       /* reorder by x */
            int tgt = (int)(((g_drag_x - g_dockr[0]) / (g_dockr[2] - g_dockr[0])) * g_dock_n);
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

/* Post-login desktop: wallpaper, three honest folder shortcuts, and a
 * full-width bottom taskbar. The right side deliberately exposes only state the
 * OS can report today; network/audio/battery indicators arrive with their
 * corresponding services instead of being decorative lies. */
static void home_ui(void) {
    g_any_active = 0; g_have_dockr = 0;   /* recomputed each frame during the build */
    Screen(.width = g_sw, .height = g_sh, .padding = -1, .align = Fill) {
        BackgroundImage("/system/images/colibri-user.ppm");
        VStack(.width = g_sw, .height = g_sh, .padding = 0, .spacing = 0,
               .align = Fill) {
            /* Reserve the top strip for our own floating menu bar (topbar.elf,
             * spawned at startup) -- its drag handle, dock and pin live there.
             * No desktop-drawn top bar anymore. */
            VStack(.width = g_sw, .height = 32, .padding = 0) { }

            /* Exact work-area height: the layout engine's `.grow` is intended
             * for siblings inside a measured row and did not consume the
             * remaining desktop height here, which left the taskbar mid-screen. */
            HStack(.padding = 16, .height = g_sh - 32 - 64, .align = Leading) {
                VStack(.spacing = 18, .align = Center) {
                    /* Desktop apps -- also the drag SOURCES: drag one onto the
                     * dock to pin it (it leaves the desktop, name and all). */
                    for (int i = 0; i < g_desk_n; i++) {
                        VStack(.spacing = 5, .width = 92, .align = Center) {
                            drag_icon(g_desk[i], 64, 1, i);
                            Text(g_desk[i].label).caption();
                        }
                    }
                }
                Spacer();
            }

            /* the app dock: a centered pill sized to its apps (not full-width) */
            HStack(.width = g_sw, .height = 64, .align = Center, .justify = Center) {
                dock_pill();
            }
        }
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
    const char *path;
    const char *start_dir;  /* distinguishes independent Files shortcuts */
    int handle_p1;
} g_running[MAX_TRACKED];

/* An app DECLARES its namespace needs in /data/apps/<name>/<name>.ns (shipped in
 * its package -- docs/USERSPACE_v2.md UP4). As the session, home reads that
 * manifest and grants the child EXACTLY those bindings, so an app runs with only
 * the subtrees it named -- naming is owning. Each line is "<ro|rw> <prefix>";
 * '#' comments and blanks are ignored. Parse into NS_BIND spawn actions; return
 * the count (0 = no manifest => the child inherits our full view, the pre-UP4
 * default, so un-manifested apps are unaffected). `desc` gets a short summary. */
#define NS_ACTS_MAX 8
static int load_app_ns(const char *elf_path,
                       struct embk_spawn_file_action *acts, int max,
                       char *desc, size_t desc_cap) {
    char mpath[256];
    size_t L = strlen(elf_path);
    if (L < 5 || L >= sizeof mpath) return 0;
    memcpy(mpath, elf_path, L + 1);
    if (strcmp(mpath + L - 4, ".elf") != 0) return 0;   /* only "<...>.elf" */
    mpath[L - 3] = 'n'; mpath[L - 2] = 's'; mpath[L - 1] = 0;   /* ".elf" -> ".ns" */

    size_t len = 0;
    uint8_t *buf = read_file(mpath, &len);
    if (!buf) return 0;
    if (!len) { free(buf); return 0; }

    int n = 0; size_t dn = 0;
    if (desc_cap) desc[0] = 0;
    for (size_t i = 0; i < len && n < max; ) {
        while (i < len && (buf[i]==' '||buf[i]=='\t'||buf[i]=='\r'||buf[i]=='\n')) i++;
        if (i >= len) break;
        if (buf[i] == '#') { while (i < len && buf[i] != '\n') i++; continue; }

        size_t ms = i;
        while (i < len && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        size_t mlen = i - ms;
        int mode;
        if      (mlen==2 && buf[ms]=='r' && buf[ms+1]=='o') mode = EMBK_NS_RO;
        else if (mlen==2 && buf[ms]=='r' && buf[ms+1]=='w') mode = EMBK_NS_RW;
        else { while (i < len && buf[i] != '\n') i++; continue; }   /* bad mode */

        while (i < len && (buf[i]==' '||buf[i]=='\t')) i++;
        size_t ps = i;
        while (i < len && buf[i]!=' ' && buf[i]!='\t' && buf[i]!='\n' && buf[i]!='\r') i++;
        size_t plen = i - ps;
        if (plen == 0 || buf[ps] != '/' || plen > 255) { while (i<len && buf[i]!='\n') i++; continue; }

        char prefix[256];
        memcpy(prefix, buf + ps, plen); prefix[plen] = 0;
        embk_action_ns_bind(&acts[n], prefix, mode);
        if (desc_cap && dn + plen + 6 < desc_cap) {
            if (dn) { desc[dn++]=','; desc[dn++]=' '; }
            desc[dn++]='r'; desc[dn++]=(mode==EMBK_NS_RO)?'o':'w'; desc[dn++]=' ';
            memcpy(desc+dn, prefix, plen); dn += plen; desc[dn]=0;
        }
        n++;
    }
    free(buf);
    return n;
}

static void spawn_app(const char *path, const char *start_dir) {
    int slot = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_running[i].path == path &&
            g_running[i].start_dir == start_dir) { slot = i; break; }
        if (slot < 0 && !g_running[i].path) slot = i;
    }
    if (slot < 0) return;

    if (g_running[slot].path == path &&
        g_running[slot].start_dir == start_dir &&
        g_running[slot].handle_p1 > 0) {
        int h = g_running[slot].handle_p1 - 1;
        if (embk_proc_alive(h)) return;   /* still running -- one instance only */
        embk_wait(h);                     /* dead: reap the zombie + free the handle */
        g_running[slot].handle_p1 = 0;
    }

    /* Grant the child EXACTLY its declared namespace (UP4); no manifest => it
     * inherits our full view. */
    struct embk_spawn_file_action acts[NS_ACTS_MAX];
    char nsdesc[224];
    int nacts = load_app_ns(path, acts, NS_ACTS_MAX, nsdesc, sizeof nsdesc);

    char *argv[] = { (char *)path, NULL };
    char file_path_env[640];
    char *launch_env[64];
    char **env = g_session_env;
    if (start_dir && start_dir[0] == '/') {
        int en = 0;
        while (g_session_env && g_session_env[en] && en < 62) {
            launch_env[en] = g_session_env[en];
            en++;
        }
        snprintf(file_path_env, sizeof file_path_env, "FILES_PATH=%s", start_dir);
        launch_env[en++] = file_path_env;
        launch_env[en] = NULL;
        env = launch_env;
    }
    int h = (int)embk_spawn_env(path, argv, env, nacts ? acts : NULL, nacts);

    char b[320];
    if (nacts) snprintf(b, sizeof b, "home: spawn %s -> ns[%s] (%d bind%s)\n",
                        path, nsdesc, nacts, nacts == 1 ? "" : "s");
    else       snprintf(b, sizeof b, "home: spawn %s -> full inherit (no manifest)\n", path);
    embk_puts(1, b);

    if (h >= 0) {
        g_running[slot].path = path;
        g_running[slot].start_dir = start_dir;
        g_running[slot].handle_p1 = h + 1;
    }
    else { char e[96]; snprintf(e, sizeof e, "home: spawn %s FAILED: %d\n", path, h); embk_puts(1, e); }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    g_session_env = envp;
    /* apps describe their own icon/name (docs presentation manifest) */
    load_app_meta("files", &g_dock[0]);
    load_app_meta("term",  &g_dock[1]);
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

    struct render_target rt = { (uint32_t *)pixels, sw, sh, sw * 4, EMBK_PIXFMT_BGRA8888_PRE };
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());

    em_set_clock(embk_uptime_ms);
    /* Our menu bar IS topbar.elf -- the Apple-modern glass bar with the drag
     * handle, draggable dock chips, and pin/snap. The desktop no longer draws
     * its own top status bar; this floating bar takes the top strip. */
    spawn_app("/data/apps/topbar/topbar.elf", NULL);
    embk_puts(1, "home: desktop ready\n");

    for (;;) {
        /* pointer: the compositor routes the desktop's content-local mouse to us */
        struct embk_win_input in;
        embk_win_input(&in);
        if (in.focused)
            ui_pointer((float)in.x, (float)in.y, (in.buttons & EMBK_MOUSE_LEFT) != 0);
        else
            ui_pointer(-100.0f, -100.0f, false);

        /* live uptime clock in the header */
        uint64_t secs = embk_uptime_ms() / 1000;
        snprintf(g_clock, sizeof g_clock, "up %lu:%02lu",
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm) strftime(g_datetime, sizeof g_datetime, "%a %d %b  %H:%M", tm);

        g_launch = 0;
        g_launch_dir = 0;
        ui_frame_begin(); em_new_frame(); home_ui(); em_flush(); ui_frame_end();
        ui_run_layout((float)sw, (float)sh);

        /* While a drag ghost is airborne (it moves every frame) or the dock just
         * changed, force a clean full repaint so no ghost/trail is left behind. */
        int force_full = (g_drag && g_drag_moved) || g_dock_dirty;
        if (force_full) { scene_render_destroy(&r); scene_render_init(&r, cpu_backend_get()); g_dock_dirty = 0; }

        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        if (force_full || r.full || r.n_dirty == 0) {
            embk_win_present(win, pixels, sw, sh);
        } else {
            int x0 = 1 << 29, y0 = 1 << 29, x1 = -(1 << 29), y1 = -(1 << 29);
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

        /* a tile was clicked this frame -> launch it as a floating window */
        if (g_launch) spawn_app(g_launch, g_launch_dir);

        embk_sleep_ms(15);   /* pace ~60Hz while YIELDING -- never starve the apps */
    }
    return 0;
}
