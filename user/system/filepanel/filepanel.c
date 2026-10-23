/* user/system/filepanel/filepanel.c -- the Open and Save panel, as a SERVICE.
 *
 * THE SHARED PIECE OF UI THIS OS DID NOT HAVE. An application that wanted a
 * file used to ask for a path typed into a text box; Note++ and the editor
 * both still shipped a field labelled "Path to open or save as". So every app
 * either wrote a browser of its own or made the user remember where things
 * live, and no two of them would have agreed about anything.
 *
 * IT IS A SEPARATE PROGRAM, not a library, and that is the design rather than
 * an implementation detail:
 *
 *   * every app gets the SAME panel -- same places, same sorting, same
 *     keyboard -- and improving it improves all of them without rebuilding a
 *     single application;
 *   * it is where this system's capability model eventually pays: the panel
 *     holds the authority to browse, the caller receives only what the user
 *     pointed at. Today that is a path (the kernel cannot wrap an open file as
 *     a passable handle yet -- user/lib/emsvc.h says why), but the shape is
 *     already the one that lets a confined app open a file it could never have
 *     found for itself;
 *   * a panel that crashes takes nothing with it. The endpoint vanishes from
 *     /run, the caller's connect fails with a normal error, and the app is
 *     still standing.
 *
 * ONE AT A TIME, deliberately. It accepts a connection, runs its window until
 * the user answers, replies, and only then accepts the next. A second app
 * asking mid-panel waits -- which is what "modal" means, and is better than
 * two pickers fighting over one keyboard. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "embk.h"
#include "emsvc.h"
#include "emfiles.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define MAX_ENTRIES 512

struct entry { char name[128]; bool is_dir; };

static struct entry g_entries[MAX_ENTRIES];
static int          g_count;
static int          g_sel = -1;
static char         g_cwd[256];
static char         g_name[128];        /* the filename field (SAVE) */
static char         g_title[48];
static int          g_mode;
static int          g_done;             /* 1 chose, -1 cancelled */
static char         g_result[256];

/* ---- the filesystem ---------------------------------------------------- */

static void join(char *out, size_t cap, const char *leaf) {
    if (!strcmp(g_cwd, "/")) snprintf(out, cap, "/%s", leaf);
    else                     snprintf(out, cap, "%s/%s", g_cwd, leaf);
}

static int cmp_entry(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return (int)y->is_dir - (int)x->is_dir;   /* folders first */
    return strcmp(x->name, y->name);
}

static void read_dir(void) {
    g_count = 0; g_sel = -1;
    DIR *d = opendir(g_cwd);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_count < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct entry *e = &g_entries[g_count];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[400];
        join(full, sizeof full, de->d_name);
        struct stat st;
        e->is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        g_count++;
    }
    closedir(d);
    qsort(g_entries, (size_t)g_count, sizeof g_entries[0], cmp_entry);
}

static void go_to(const char *path) {
    snprintf(g_cwd, sizeof g_cwd, "%s", path && path[0] ? path : "/");
    read_dir();
}

static void go_up(void) {
    if (!strcmp(g_cwd, "/")) return;
    char *slash = strrchr(g_cwd, '/');
    if (!slash) return;
    if (slash == g_cwd) g_cwd[1] = 0;       /* "/x" -> "/" */
    else                *slash = 0;
    read_dir();
}

/* Finish: the chosen path, or nothing. */
static void choose(const char *leaf) {
    char full[256];
    if (!strcmp(g_cwd, "/")) snprintf(full, sizeof full, "/%s", leaf);
    else                     snprintf(full, sizeof full, "%s/%s", g_cwd, leaf);
    snprintf(g_result, sizeof g_result, "%s", full);
    g_done = 1;
    em_app_request_exit(0);
}

/* ---- the panel --------------------------------------------------------- */

/* The places every panel offers. Not a user preference yet -- these are the
 * directories this OS's layout guarantees exist (docs/USERSPACE_v2.md), so a
 * panel that offers them cannot offer a dead link. */
static const struct { const char *label; const char *path; } g_places[] = {
    { "Home",      "/home" },
    { "Documents", "/home" },
    { "Data",      "/data" },
    { "System",    "/system" },
    { "Root",      "/" },
};

static void panel(void) {
    const struct ui_theme *t = ui_theme();

    Window(g_title[0] ? g_title : (g_mode == EMFILE_SAVE ? "Save" : "Open")) {
        AppBar(g_title[0] ? g_title : (g_mode == EMFILE_SAVE ? "Save As" : "Open"));

        HStack(.grow = 1, .align = Fill, .spacing = 0) {
            /* places */
            VStack(.width = 150, .align = Fill, .spacing = 2, .padding = 8,
                   .background = t->surface_alt) {
                Text("Places").caption().secondary();
                for (unsigned i = 0; i < sizeof g_places / sizeof g_places[0]; i++) {
                    if (Button(g_places[i].label).ghost().id(g_places[i].label).clicked())
                        go_to(g_places[i].path);
                }
            }

            VStack(.grow = 1, .align = Fill, .spacing = 0) {
                /* where we are, and the way back up */
                HStack(.spacing = 8, .align = Center, .padding = 8) {
                    if (Button("Up").ghost().id("up").clicked()) go_up();
                    Text(g_cwd).caption().secondary();
                }
                Divider();

                /* the listing */
                static float scroll;
                ScrollView(&scroll, 0, .grow = 1) {
                    VStack(.align = Fill, .spacing = 1, .padding = 6) {
                        static const char *KEY[MAX_ENTRIES];
                        for (int i = 0; i < g_count; i++) {
                            KEY[i] = g_entries[i].name;
                            bool sel = (i == g_sel);
                            HStack(.spacing = 8, .align = Center, .px = 8, .py = 5,
                                   .corner = t->radius_sm, .key = KEY[i],
                                   .background = sel ? t->accent_soft : (struct color){0,0,0,0}) {
                                Icon(g_entries[i].is_dir ? IconFolder : IconDoc)
                                    .color(g_entries[i].is_dir ? t->accent : t->text_secondary);
                                Text(g_entries[i].name);
                            }
                            if (Clicked(KEY[i])) {
                                if (g_entries[i].is_dir) {
                                    char full[256];
                                    join(full, sizeof full, g_entries[i].name);
                                    go_to(full);
                                } else {
                                    g_sel = i;
                                    snprintf(g_name, sizeof g_name, "%s", g_entries[i].name);
                                }
                            }
                        }
                        if (g_count == 0) EmptyState(IconFolder, "Nothing here", "");
                    }
                }

                Divider();
                /* the answer, and the two buttons that end this */
                HStack(.spacing = 8, .align = Center, .padding = 10) {
                    if (g_mode == EMFILE_SAVE) {
                        Text("Save as").caption().secondary();
                        TextField(g_name, sizeof g_name, "Filename");
                    } else {
                        Text(g_name[0] ? g_name : "Select a file").caption().secondary();
                    }
                    Spacer();
                    if (Button("Cancel").id("cancel").clicked()) {
                        g_done = -1; em_app_request_exit(0);
                    }
                    if (Button(g_mode == EMFILE_SAVE ? "Save" : "Open")
                            .primary().id("ok").clicked()) {
                        if (g_name[0]) choose(g_name);
                    }
                }
            }
        }
    }
}

/* ---- the service loop -------------------------------------------------- */

static EmApp g_spec = {
    .title  = "Open",
    .size   = { 760, 460 },
    .theme  = Dark,
    .chrome = Chromeless,
    .view   = panel,
};

int main(void) {
    int lh = emsvc_listen("files");
    if (lh < 0) {
        /* SAY SO. A panel service that cannot publish its endpoint leaves
         * every future "Open" in the system failing with -ENOENT, and without
         * this line nothing would connect that to a missing process. */
        char b[96];
        snprintf(b, sizeof b, "filepanel: cannot listen at /run/emlink.files (%d)\n", lh);
        embk_puts(1, b);
        return 1;
    }
    embk_puts(1, "filepanel: serving /run/emlink.files\n");

    for (;;) {
        struct emfile_req q;
        uint32_t type = 0;
        unsigned len  = 0;
        int ch = emsvc_accept(lh, &type, &q, sizeof q, &len);
        if (ch < 0) continue;
        if (type != EMSVC_T_FILE_PANEL || len < sizeof q) {
            struct emfile_rep bad = { 0, -1, { 0 } };
            emsvc_reply(ch, type, &bad, sizeof bad);
            continue;
        }

        g_mode = (int)q.mode;
        snprintf(g_title, sizeof g_title, "%s", q.title);
        snprintf(g_name,  sizeof g_name,  "%s", q.suggest);
        go_to(q.start[0] ? q.start : "/home");
        g_done = 0;
        g_result[0] = 0;
        g_spec.title = g_mode == EMFILE_SAVE ? "Save" : "Open";

        em_app_run(&g_spec);     /* the window, until the user answers */

        struct emfile_rep r;
        memset(&r, 0, sizeof r);
        r.chosen = (g_done == 1) ? 1 : 0;
        r.handle = -1;           /* reserved: see emsvc.h */
        if (r.chosen) snprintf(r.path, sizeof r.path, "%s", g_result);
        emsvc_reply(ch, EMSVC_T_FILE_PANEL, &r, sizeof r);
    }
}
