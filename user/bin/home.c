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
static char        g_clock[32] = "up 0:00";
static float       g_sw = 0, g_sh = 0;   /* screen size (the desktop window) */

/* One launcher tile. `path` != NULL is a real, launchable app (accent tile);
 * NULL is a "coming soon" placeholder (a dim, non-launching tile). Both are the
 * same size so the grid stays even. */
static void tile(const char *name, const char *path) {
    if (path) {
        if (Button(name).primary().width(150).height(96).clicked()) g_launch = path;
    } else {
        Button(name).secondary().width(150).height(96).id(name);
    }
}

/* The whole home screen: a centred column -- header + a 2x3 tile grid -- filling
 * the whole desktop window (sized explicitly to it, so the bg covers the screen
 * and the content is truly centred). */
static void home_ui(void) {
    Screen(.width = g_sw, .height = g_sh, .justify = Center, .align = Center) {
        VStack(.spacing = 30, .align = Center) {
            VStack(.spacing = 6, .align = Center) {
                Text("EmbLink OS").title();
                Text(g_clock).caption().secondary();
            }
            VStack(.spacing = 16, .align = Center) {
                HStack(.spacing = 16) {
                    tile("Files",    "/data/apps/files/files.elf");
                    tile("Editor",   "/data/apps/edit/edit.elf");
                    tile("Terminal", "/data/apps/term/term.elf");
                }
                HStack(.spacing = 16) {
                    tile("UI Demo",  "/data/apps/uidemo/uidemo.elf");
                    tile("Windows",  "/data/apps/wmdemo/wmdemo.elf");
                    tile("Menus",    "/data/apps/v6demo/v6demo.elf");
                }
            }
        }
    }
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
static struct { const char *path; int handle_p1; } g_running[MAX_TRACKED];

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

static void spawn_app(const char *path) {
    int slot = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_running[i].path == path) { slot = i; break; }
        if (slot < 0 && !g_running[i].path) slot = i;
    }
    if (slot < 0) return;

    if (g_running[slot].path == path && g_running[slot].handle_p1 > 0) {
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
    int h = (int)embk_spawn(path, argv, nacts ? acts : NULL, nacts);

    char b[320];
    if (nacts) snprintf(b, sizeof b, "home: spawn %s -> ns[%s] (%d bind%s)\n",
                        path, nsdesc, nacts, nacts == 1 ? "" : "s");
    else       snprintf(b, sizeof b, "home: spawn %s -> full inherit (no manifest)\n", path);
    embk_puts(1, b);

    if (h >= 0) { g_running[slot].path = path; g_running[slot].handle_p1 = h + 1; }
    else { char e[96]; snprintf(e, sizeof e, "home: spawn %s FAILED: %d\n", path, h); embk_puts(1, e); }
}

int main(void) {
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
    spawn_app("/data/apps/clockw/clockw.elf");   /* the desktop CLOCK widget (V5) -- fire & track */
    spawn_app("/data/apps/topbar/topbar.elf");   /* the dynamic Apple-modern menu bar */
    embk_puts(1, "home: launcher up; click a tile to launch an app\n");

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

        g_launch = 0;
        ui_frame_begin(); em_new_frame(); home_ui(); em_flush(); ui_frame_end();
        ui_run_layout((float)sw, (float)sh);
        scene_render_frame(&r, &sa, ui_scene_of(ui_root()), &rt);

        if (r.full || r.n_dirty == 0) {
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
        if (g_launch) spawn_app(g_launch);

        embk_sleep_ms(15);   /* pace ~60Hz while YIELDING -- never starve the apps */
    }
    return 0;
}
