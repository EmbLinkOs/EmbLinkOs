/* user/apps/edit/edit.c -- a REAL text editor: open a file, edit it, SAVE it.
 *
 * Unlike v7demo (a TextEditor showcase over a fixed string), this reads and
 * writes an actual file. The path comes from the environment variable
 * EDIT_FILE, which the Files app sets when you open a file; launched on its own
 * (from the home tile) it falls back to a scratch file, so "Editor" always
 * opens something you can type into and Save.
 *
 * Persistence is real fopen/fread + fopen("w")/fwrite -- reopen the file (or
 * reboot with a writethrough disk) and your text is still there.
 */
#include <stdio.h>
#include <stdlib.h>     /* getenv */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "emfiles.h"   /* the system Open/Save panel */
#include "ui.h"
#include "em.h"
#include "theme.h"

#define DOC_MAX 32768

static char g_path[256];
static char g_doc[DOC_MAX];
static int  g_cur = 0;
static char g_status[96] = "";
static bool g_loaded = false;

/* basename, for the window title + header */
static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (*s == '/') b = s + 1;
    return *b ? b : p;
}

static void load_file(void) {
    const char *p = getenv("EDIT_FILE");
    if (!p || !*p) p = "/data/tmp/untitled.txt";
    snprintf(g_path, sizeof g_path, "%s", p);

    FILE *f = fopen(g_path, "r");
    if (f) {
        size_t n = fread(g_doc, 1, DOC_MAX - 1, f);
        g_doc[n] = 0;
        fclose(f);
        snprintf(g_status, sizeof g_status, "%lu bytes", (unsigned long)n);
    } else {
        g_doc[0] = 0;
        snprintf(g_status, sizeof g_status, "new file");
    }
    g_cur = 0;
}

static void save_file(void) {
    FILE *f = fopen(g_path, "w");
    if (!f) { snprintf(g_status, sizeof g_status, "SAVE FAILED (open)"); return; }
    size_t len = strlen(g_doc);
    size_t w = fwrite(g_doc, 1, len, f);
    fclose(f);
    if (w == len) snprintf(g_status, sizeof g_status, "saved -- %lu bytes", (unsigned long)w);
    else          snprintf(g_status, sizeof g_status, "PARTIAL WRITE (%lu/%lu)",
                           (unsigned long)w, (unsigned long)len);
}

/* The directory part of a path, for opening the panel where the user already
 * is rather than at a fixed place. */
static const char *dir_of(const char *path) {
    static char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = dir;
    for (char *q = dir; *q; q++) if (*q == '/') slash = q;
    if (slash == dir) dir[1] = 0; else *slash = 0;
    return dir[0] ? dir : "/";
}

static void app(void) {
    if (!g_loaded) { load_file(); g_loaded = true; }

    Window("Editor") {
        WindowBar(base_name(g_path)) {
            CloseGrip();   /* pull to dismiss -- runtime handles teardown */
        }
        VStack(.spacing = 10, .padding = 16, .align = Fill) {
            HStack(.spacing = 12, .align = Fill) {
                Text(base_name(g_path)).heading();
                Spacer();
                Text(g_status).caption().secondary();
                /* OPEN AND SAVE AS, through the SYSTEM panel -- one call, and
                 * the same picker every other application gets. This editor
                 * had neither: it could only ever edit the file it was handed
                 * on the command line, because writing a file browser is a lot
                 * of app for a program this size. That is exactly the work a
                 * shared service is supposed to take off an application. */
                if (Button("Open").ghost().clicked()) {
                    char picked[256];
                    int r = embk_file_panel(EMFILE_OPEN, "Open", dir_of(g_path),
                                            0, picked, sizeof picked);
                    if (r == 1) {
                        snprintf(g_path, sizeof g_path, "%s", picked);
                        g_loaded = false;     /* load_file() on the next frame */
                    } else if (r < 0) {
                        snprintf(g_status, sizeof g_status,
                                 "no file panel (%d)", r);
                    }
                }
                if (Button("Save As").ghost().clicked()) {
                    char picked[256];
                    int r = embk_file_panel(EMFILE_SAVE, "Save As", dir_of(g_path),
                                            base_name(g_path), picked, sizeof picked);
                    if (r == 1) {
                        snprintf(g_path, sizeof g_path, "%s", picked);
                        save_file();
                    } else if (r < 0) {
                        snprintf(g_status, sizeof g_status,
                                 "no file panel (%d)", r);
                    }
                }
                if (Button("Save").primary().clicked())
                    save_file();
            }
            TextEditor(g_doc, sizeof g_doc, &g_cur, 380);
        }
    }
}

EM_APPLICATION {
    .title  = "Editor",
    .size   = { 620, 520 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};
