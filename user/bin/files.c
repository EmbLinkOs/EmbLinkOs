/* user/bin/files.c -- the file manager.
 *
 * Rebuilt on the shared application chrome (AppBar): traffic lights leading, a
 * centred title, the app's own controls trailing -- the same frame Settings and
 * the Terminal wear, so the three read as one product rather than three demos.
 *
 * The structure is the one file managers have had for thirty years, because it
 * answers the three questions in order: a places sidebar ("where can I go"), a
 * toolbar ("where am I, how do I get back"), the listing, and a status line
 * ("what am I looking at"). What matters is in the details:
 *
 *   - Search FILTERS the folder you are in as you type. A search that navigates
 *     somewhere else has thrown away the place you were standing.
 *   - Two views, because they answer different questions: Grid is for
 *     recognising things by shape, List for comparing them by size and kind.
 *     Neither is the better default, so both are one click away.
 *   - Sizes are human ("1.4 MB", not 1468006) and kinds are named ("Program",
 *     not ".elf"). Showing raw bytes is making the reader do arithmetic the
 *     program could have done.
 *   - New Folder really creates one. Chrome that does nothing is worse than
 *     chrome that is absent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define MAX_ENTRIES 512
#define NAME_LEN    112

struct entry { char name[NAME_LEN]; bool is_dir; long size; };

static struct entry g_entries[MAX_ENTRIES];
static int   g_count = 0;                 /* entries in the directory         */
static int   g_vis[MAX_ENTRIES];          /* indices passing the filter       */
static int   g_vis_n = 0;
static char  g_cwd[512] = "/";
static bool  g_dirty = true;              /* re-read the directory this frame */
static bool  g_initialized = false;
static float g_scroll = 0;
static char  g_status[128] = "";
static char  g_query[64] = "";            /* the live filter                  */
static int   g_view = 0;                  /* 0 = grid, 1 = list               */
static int   g_sel = -1;

/* Back AND forward: "back" on its own is half a history. */
static char  g_back[24][512];  static int g_back_n;
static char  g_fwd[24][512];   static int g_fwd_n;

/* New Folder names first, then creates, rather than dropping an "untitled
 * folder" you rename afterwards -- there is no rename yet, so an untitled
 * folder would be permanent. */
static bool  g_naming = false;
static char  g_newname[NAME_LEN] = "";

/* ---- model ------------------------------------------------------------- */

static void join(char *out, size_t cap, const char *name) {
    if (strcmp(g_cwd, "/") == 0) snprintf(out, cap, "/%s", name);
    else                         snprintf(out, cap, "%s/%s", g_cwd, name);
}

static int cmp_entry(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return (int)y->is_dir - (int)x->is_dir;  /* folders first */
    return strcmp(x->name, y->name);
}

static void read_dir(void) {
    g_count = 0; g_sel = -1;
    DIR *d = opendir(g_cwd);
    if (!d) { snprintf(g_status, sizeof g_status, "Cannot open this location"); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_count < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct entry *e = &g_entries[g_count];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[600];
        join(full, sizeof full, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) { e->is_dir = S_ISDIR(st.st_mode); e->size = (long)st.st_size; }
        else                      { e->is_dir = false; e->size = 0; }
        g_count++;
    }
    closedir(d);
    qsort(g_entries, g_count, sizeof g_entries[0], cmp_entry);
    g_status[0] = 0;
}

/* case-insensitive substring -- search should not care about Shift */
static bool matches(const char *name) {
    if (!g_query[0]) return true;
    for (const char *p = name; *p; p++) {
        const char *a = p, *b = g_query;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

/* Resolve the filter ONCE per frame into an index list. The grid needs to lay
 * out rows of N, and doing that over a predicate mid-layout is how you end up
 * with a loop that is hard to read and easy to get wrong. */
static void build_visible(void) {
    g_vis_n = 0;
    for (int i = 0; i < g_count; i++)
        if (matches(g_entries[i].name)) g_vis[g_vis_n++] = i;
}

static void navigate_to(const char *path) {
    if (!path || path[0] != '/' || !strcmp(path, g_cwd)) return;
    if (g_back_n == 24) { memmove(g_back, g_back + 1, sizeof g_back - sizeof g_back[0]); g_back_n--; }
    snprintf(g_back[g_back_n++], sizeof g_back[0], "%s", g_cwd);
    g_fwd_n = 0;                         /* a new branch discards the forward arm */
    snprintf(g_cwd, sizeof g_cwd, "%s", path);
    g_dirty = true; g_scroll = 0; g_query[0] = 0; g_naming = false;
}

static void go_back(void) {
    if (g_back_n <= 0) return;
    if (g_fwd_n < 24) snprintf(g_fwd[g_fwd_n++], sizeof g_fwd[0], "%s", g_cwd);
    snprintf(g_cwd, sizeof g_cwd, "%s", g_back[--g_back_n]);
    g_dirty = true; g_scroll = 0; g_query[0] = 0;
}

static void go_forward(void) {
    if (g_fwd_n <= 0) return;
    if (g_back_n < 24) snprintf(g_back[g_back_n++], sizeof g_back[0], "%s", g_cwd);
    snprintf(g_cwd, sizeof g_cwd, "%s", g_fwd[--g_fwd_n]);
    g_dirty = true; g_scroll = 0; g_query[0] = 0;
}

static void go_up(void) {
    if (!strcmp(g_cwd, "/")) return;
    char parent[512];
    snprintf(parent, sizeof parent, "%s", g_cwd);
    char *slash = strrchr(parent, '/');
    if (slash == parent) parent[1] = 0; else *slash = 0;
    navigate_to(parent);
}

static void enter_dir(const char *name) {
    char nc[512]; join(nc, sizeof nc, name); navigate_to(nc);
}

static void open_file(const char *name) {
    char full[600], envbuf[640];
    join(full, sizeof full, name);
    snprintf(envbuf, sizeof envbuf, "EDIT_FILE=%s", full);
    char *argv[] = { "edit", NULL };
    char *env[]  = { envbuf, NULL };
    int64_t h = embk_spawn_env("/data/apps/edit/edit.elf", argv, env, NULL, 0);
    snprintf(g_status, sizeof g_status, h >= 0 ? "Opened %s" : "Could not open %s", name);
}

static void create_folder(void) {
    if (!g_newname[0]) { g_naming = false; return; }
    char full[600];
    join(full, sizeof full, g_newname);
    if (embk_mkdir(full) >= 0) snprintf(g_status, sizeof g_status, "Created %s", g_newname);
    else                       snprintf(g_status, sizeof g_status, "Could not create %s here", g_newname);
    g_newname[0] = 0; g_naming = false; g_dirty = true;
}

/* ---- naming things ----------------------------------------------------- */

static const char *human_size(long n, char *buf, size_t cap) {
    if (n < 1024)            snprintf(buf, cap, "%ld B", n);
    else if (n < 1024L*1024) snprintf(buf, cap, "%ld KB", (n + 512) / 1024);
    else                     snprintf(buf, cap, "%ld.%ld MB", n / (1024L*1024),
                                      ((n % (1024L*1024)) * 10) / (1024L*1024));
    return buf;
}

static const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? dot + 1 : "";
}

/* A named kind beats an extension: people look for "the program", not ".elf". */
static const char *kind_of(const struct entry *e) {
    if (e->is_dir) return "Folder";
    const char *x = ext_of(e->name);
    if (!strcmp(x, "elf") || !strcmp(x, "embx")) return "Program";
    if (!strcmp(x, "so"))                        return "Library";
    if (!strcmp(x, "c") || !strcmp(x, "h"))      return "Source";
    if (!strcmp(x, "txt") || !strcmp(x, "md"))   return "Text";
    if (!strcmp(x, "eic") || !strcmp(x, "png"))  return "Image";
    if (!strcmp(x, "ttf"))                       return "Font";
    if (!strcmp(x, "ns") || !strcmp(x, "app") || !strcmp(x, "conf")) return "Settings";
    if (!strcmp(x, "img") || !strcmp(x, "pkg"))  return "Archive";
    if (!x[0])                                   return "Document";
    return x;
}

static int icon_for(const struct entry *e) {
    if (e->is_dir) return IconFolder;
    const char *k = kind_of(e);
    if (!strcmp(k, "Program"))  return IconBolt;
    if (!strcmp(k, "Image"))    return IconGrid;
    if (!strcmp(k, "Settings")) return IconGear;
    return IconDoc;
}

/* ---- pieces ------------------------------------------------------------ */

/* Ghost buttons paint their label in the ACCENT, which is right for an action
 * and wrong for a name. A window where every filename and every place is
 * accent-blue reads as a page of hyperlinks: the accent stops meaning
 * "this one" when everything wears it. So names are text-coloured, and the
 * accent is spent on exactly one thing -- the place you are actually in. */
static void sidebar_item(int icon, const char *label, const char *path) {
    const struct ui_theme *t = ui_theme();
    bool here = path && !strcmp(path, g_cwd);
    HStack(.spacing = 9, .align = Center, .px = 8, .py = 4, .corner = 7, .grow = 1,
           .background = here ? (Color){ .r=.20f, .g=.22f, .b=.30f, .a=1.f }
                              : (Color){ 0, 0, 0, 0 }) {
        Icon(icon).color(here ? t->accent : t->text_secondary);
        if (Button(label).ghost().color(here ? t->accent : t->text)
                         .grow().leading().clicked()) navigate_to(path);
    }
}

static void grid_cell(int i) {
    struct entry *e = &g_entries[i];
    VStack(.spacing = 4, .width = 116, .align = Center) {
        if (e->is_dir) {
            /* one bitmap, many folders -- each needs its own interaction
             * identity or they share a hover state */
            if (ImageButtonKey("/system/images/file.eic", 58, e)) enter_dir(e->name);
        } else {
            if (IconButton(icon_for(e)).frame(58, 58).font(Title).clicked()) open_file(e->name);
        }
        if (Button(e->name).ghost().color(ui_theme()->text).width(112).clicked()) {
            if (e->is_dir) enter_dir(e->name); else open_file(e->name);
        }
    }
}

static void list_row(int i) {
    struct entry *e = &g_entries[i];
    char sz[24];
    bool sel = (g_sel == i);
    HStack(.spacing = 12, .align = Center, .px = 10, .py = 4, .corner = 6, .grow = 1,
           .background = sel ? (Color){ .r=.20f, .g=.22f, .b=.30f, .a=1.f }
                             : (Color){ 0, 0, 0, 0 }) {
        Icon(icon_for(e)).secondary();
        if (Button(e->name).ghost().color(ui_theme()->text).grow().leading().clicked()) {
            g_sel = i;
            if (e->is_dir) enter_dir(e->name); else open_file(e->name);
        }
        Text(kind_of(e)).caption().secondary();
        /* Sizes trail so the digits line up down the column and can be compared
         * without reading every one of them. */
        Text(e->is_dir ? "--" : human_size(e->size, sz, sizeof sz)).caption().tertiary();
    }
}

/* ---- the window -------------------------------------------------------- */

static void app(void) {
    if (!g_initialized) {
        const char *start = getenv("FILES_PATH");
        if (!start || start[0] != '/') start = getenv("HOME");
        if (start && start[0] == '/') snprintf(g_cwd, sizeof g_cwd, "%s", start);
        g_initialized = true;
    }
    if (g_dirty) { read_dir(); g_dirty = false; }
    build_visible();

    const char *home = getenv("HOME");
    if (!home || home[0] != '/') home = "/";
    char desktop[560], documents[560], downloads[560], music[560];
    char pictures[560], videos[560], trash[560];
    snprintf(desktop,   sizeof desktop,   "%s/Desktop",   home);
    snprintf(documents, sizeof documents, "%s/Documents", home);
    snprintf(downloads, sizeof downloads, "%s/Downloads", home);
    snprintf(music,     sizeof music,     "%s/Music",     home);
    snprintf(pictures,  sizeof pictures,  "%s/Pictures",  home);
    snprintf(videos,    sizeof videos,    "%s/Videos",    home);
    snprintf(trash,     sizeof trash,     "%s/Trash",     home);

    static const char *views[] = { "Grid", "List" };

    Window("Files") {
        AppBar("Files") {
            SearchField(g_query, sizeof g_query, "Search this folder");
            Segmented(views, 2, &g_view);
        }

        Split(212) {
            SidebarPane() {
                Text("Favourites").caption().tertiary();
                sidebar_item(IconHome,   "Home",      home);
                sidebar_item(IconGrid,   "Desktop",   desktop);
                sidebar_item(IconDoc,    "Documents", documents);
                sidebar_item(IconArrowR, "Downloads", downloads);
                sidebar_item(IconList,   "Music",     music);
                sidebar_item(IconFiles,  "Pictures",  pictures);
                sidebar_item(IconGrid,   "Videos",    videos);
                Divider();
                Text("Locations").caption().tertiary();
                sidebar_item(IconFiles, "System", "/system");
                sidebar_item(IconFiles, "Data",   "/data");
                sidebar_item(IconFiles, "Root",   "/");
                Divider();
                sidebar_item(IconTrash, "Trash", trash);
                Spacer();
                Text("EmbLink OS").caption().tertiary();
            }

            ContentPane(.padding = 0) {
                /* toolbar: where am I, and how do I get out of here */
                HStack(.spacing = 8, .align = Center, .px = 14, .py = 8) {
                    if (IconButton(IconChevronL).clicked()) go_back();
                    if (IconButton(IconChevronR).clicked()) go_forward();
                    if (IconButton(IconChevronU).clicked()) go_up();
                    Card(.padding = 7, .grow = 1, .corner = 7,
                         .background = { .r=.12f, .g=.125f, .b=.145f, .a=1.f }) {
                        Text(g_cwd).body();
                    }
                    if (g_naming) {
                        TextField(g_newname, sizeof g_newname, "Folder name");
                        if (Button("Create").primary().clicked()) create_folder();
                        if (Button("Cancel").ghost().clicked()) { g_naming = false; g_newname[0] = 0; }
                    } else {
                        if (Button("New Folder").ghost().color(ui_theme()->text).clicked()) {
                            g_naming = true; g_newname[0] = 0;
                        }
                    }
                }
                Divider();

                /* The scroll viewport is measured, not guessed. A constant
                 * height is a claim about the window size, and the moment the
                 * window is smaller than the claim the status line below gets
                 * pushed off the bottom edge -- chrome silently lost to a
                 * number written months earlier. Reserve the bar, the toolbar
                 * and the status line; the rest is the list. */
                ScrollView(&g_scroll, em_viewport_height() - 150.0f) {
                    VStack(.spacing = g_view ? 2 : 14, .align = Fill, .padding = 14) {
                        if (g_vis_n == 0) {
                            if (g_query[0])
                                EmptyState(IconSearch, "No matches",
                                           "Nothing in this folder matches what you typed.");
                            else
                                EmptyState(IconFolder, "Empty folder",
                                           "There is nothing here yet.");
                        } else if (g_view == 1) {
                            for (int k = 0; k < g_vis_n; k++) list_row(g_vis[k]);
                        } else {
                            int cols = ((int)em_viewport_width() - 250) / 128;
                            if (cols < 2) cols = 2;
                            if (cols > 8) cols = 8;
                            for (int base = 0; base < g_vis_n; base += cols) {
                                HStack(.spacing = 12, .align = Leading) {
                                    for (int c = 0; c < cols; c++) {
                                        int k = base + c;
                                        if (k < g_vis_n) grid_cell(g_vis[k]);
                                        else             Spacer();
                                    }
                                }
                            }
                        }
                    }
                }

                /* status line: what am I looking at */
                Divider();
                HStack(.spacing = 10, .align = Center, .px = 14, .py = 6) {
                    char line[96];
                    if (g_query[0]) snprintf(line, sizeof line, "%d of %d items", g_vis_n, g_count);
                    else            snprintf(line, sizeof line, "%d items", g_count);
                    Text(line).caption().secondary();
                    Spacer();
                    if (g_status[0]) Text(g_status).caption().tertiary();
                }
            }
        }
    }
}

EM_APPLICATION {
    .title  = "Files",
    /* Fits the work area: 768 less the 26px menu bar and the dock's band.
     * A default window that runs under the dock is a window that ships with
     * its own status bar hidden. */
    .size   = { 1000, 600 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};
