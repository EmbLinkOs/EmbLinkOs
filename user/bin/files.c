/* user/bin/files.c -- a file manager: browse the filesystem, see sizes, open
 * files in the Editor.
 *
 * Reads directories with opendir/readdir, sizes/types with stat, sorts folders
 * first, and on a file tap launches /data/apps/edit/edit.elf with
 * EDIT_FILE=<full path> in its environment (edit.c reads that). Folders drill
 * in; "Up" climbs. A concrete, usable window onto EMBKFS + /system + /data --
 * and the click-to-open half of a real create/browse/edit/save loop.
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

#define MAX_ENTRIES 256
#define NAME_LEN    112

struct entry { char name[NAME_LEN]; bool is_dir; long size; };

static struct entry g_entries[MAX_ENTRIES];
static int   g_count = 0;
static char  g_cwd[512] = "/";
static bool  g_dirty = true;      /* re-read the directory this frame */
static float g_scroll = 0;
static char  g_status[96] = "";

/* Build "<cwd>/<name>" without doubling the root slash. */
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
    g_count = 0;
    DIR *d = opendir(g_cwd);
    if (!d) { snprintf(g_status, sizeof g_status, "cannot open"); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
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
    snprintf(g_status, sizeof g_status, "%d items", g_count);

    char line[128];
    snprintf(line, sizeof line, "Files: %s -> %d items\n", g_cwd, g_count);
    embk_puts(1, line);
}

static void enter_dir(const char *name) {
    char nc[512];
    join(nc, sizeof nc, name);
    snprintf(g_cwd, sizeof g_cwd, "%s", nc);
    g_dirty = true; g_scroll = 0;
}

static void go_up(void) {
    if (strcmp(g_cwd, "/") == 0) return;
    char *slash = strrchr(g_cwd, '/');
    if (slash == g_cwd) g_cwd[1] = 0;   /* parent is the root */
    else                *slash = 0;
    g_dirty = true; g_scroll = 0;
}

static void open_file(const char *name) {
    char full[600], envbuf[640];
    join(full, sizeof full, name);
    snprintf(envbuf, sizeof envbuf, "EDIT_FILE=%s", full);

    char *argv[] = { "edit", NULL };
    char *env[]  = { envbuf, NULL };
    int64_t h = embk_spawn_env("/data/apps/edit/edit.elf", argv, env, NULL, 0);
    if (h >= 0) snprintf(g_status, sizeof g_status, "opened %s", name);
    else        snprintf(g_status, sizeof g_status, "open failed");
}

static void human(long n, char *out, size_t cap) {
    if      (n < 1024)        snprintf(out, cap, "%ld B",  n);
    else if (n < 1024 * 1024) snprintf(out, cap, "%ld KB", n / 1024);
    else                      snprintf(out, cap, "%ld MB", n / (1024 * 1024));
}

static void app(void) {
    if (g_dirty) { read_dir(); g_dirty = false; }

    Window("Files") {
        WindowBar("Files") {
            CloseGrip();
        }
        VStack(.spacing = 8, .padding = 16, .align = Fill) {
            HStack(.spacing = 12, .align = Fill) {
                Text(g_cwd).heading();
                Spacer();
                Text(g_status).caption().secondary();
            }
            HStack(.spacing = 8) {
                if (Button("Up").clicked()) go_up();
            }
            ScrollView(&g_scroll, 420) {
                List() {
                    for (int i = 0; i < g_count; i++) {
                        char val[32];
                        if (g_entries[i].is_dir) snprintf(val, sizeof val, "folder");
                        else                     human(g_entries[i].size, val, sizeof val);

                        int icon = g_entries[i].is_dir ? IconFolder : IconDoc;
                        if (ListRow(icon, g_entries[i].name, val)
                                .id(g_entries[i].name).clicked()) {
                            if (g_entries[i].is_dir) enter_dir(g_entries[i].name);
                            else                     open_file(g_entries[i].name);
                        }
                    }
                }
            }
        }
    }
}

EM_APPLICATION {
    .title  = "Files",
    .size   = { 540, 600 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};
