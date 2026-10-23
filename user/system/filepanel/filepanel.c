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
#include "fileops.h"
#include <errno.h>
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
static char g_error[112], g_home[256], g_folder[96];
static int g_replace, g_new_folder;
static float g_scroll;

/* ---- the filesystem ---------------------------------------------------- */

static void join(char *out, size_t cap, const char *leaf) {
    int n=snprintf(out,cap,"%s%s%s",g_cwd,!strcmp(g_cwd,"/")?"":"/",leaf);
    if(n<0 || n>=(int)cap) out[0]=0;
}

static int cmp_entry(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return (int)y->is_dir - (int)x->is_dir;   /* folders first */
    return strcmp(x->name, y->name);
}

static void read_dir(void) {
    g_count = 0; g_sel = -1;
    DIR *d = opendir(g_cwd);
    if (!d) { snprintf(g_error,sizeof g_error,"Cannot read this folder"); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_count < MAX_ENTRIES) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct entry *e = &g_entries[g_count];
        if(strlen(de->d_name)>=sizeof e->name)continue;
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
    struct stat st;
    if(!path || strlen(path)>=sizeof g_cwd || stat(path,&st) || !S_ISDIR(st.st_mode)) {
        snprintf(g_error,sizeof g_error,"Folder is unavailable");return;
    }
    snprintf(g_cwd,sizeof g_cwd,"%s",path);g_error[0]=0;g_name[0]=0;g_scroll=0;
    read_dir();em_request_frame();
}

static void go_up(void) {
    if (!strcmp(g_cwd, "/")) return;
    char *slash = strrchr(g_cwd, '/');
    if (!slash) return;
    if (slash == g_cwd) g_cwd[1] = 0;       /* "/x" -> "/" */
    else                *slash = 0;
    if(g_mode==EMFILE_OPEN)g_name[0]=0;
    g_scroll=0;read_dir();em_request_frame();
}

/* An existing destination requires an explicit Replace answer. A name is a
 * leaf, never a path: slashes and dot segments cannot escape the chosen folder. */
static void finish(void) {
    g_done=1;em_app_request_exit(0);
}
static void choose(const char *leaf) {
    if(!embk_file_valid_name(leaf)) {
        snprintf(g_error,sizeof g_error,"Enter a filename without / or control characters");return;
    }
    join(g_result,sizeof g_result,leaf);
    if(!g_result[0]) {snprintf(g_error,sizeof g_error,"Path is too long");return;}
    struct stat st;int rc=stat(g_result,&st);
    if(!rc && !S_ISREG(st.st_mode)) {snprintf(g_error,sizeof g_error,"Select a regular file");return;}
    if(rc && (g_mode==EMFILE_OPEN || errno!=ENOENT)) {
        snprintf(g_error,sizeof g_error,"File is unavailable");return;
    }
    if(g_mode==EMFILE_SAVE && !rc) {g_replace=1;return;}
    finish();
}
static int on_key(int ch) {
    if(ch==27 && (g_replace || g_new_folder)) {
        g_replace=g_new_folder=0;return 1;
    }
    if(ch=='\n' && !ui_any_focus() && !g_replace && !g_new_folder) {
        if(g_name[0])choose(g_name);
        return 1;
    }
    return 0;
}
static const char *place_path(unsigned index) {
    static char path[320];
    static const char *sub[]={"","Documents","Downloads","Desktop"};
    snprintf(path,sizeof path,"%s%s%s",g_home,index?"/":"",sub[index]);return path;
}

static void panel(void) {
    const struct ui_theme *t = ui_theme();

    Window(g_title[0] ? g_title : (g_mode == EMFILE_SAVE ? "Save" : "Open")) {
        AppBar(g_title[0] ? g_title : (g_mode == EMFILE_SAVE ? "Save As" : "Open"));

        HStack(.grow = 1, .align = Fill, .spacing = 0) {
            /* places */
            VStack(.width = 150, .align = Fill, .spacing = 2, .padding = 8,
                   .background = t->sidebar) {
                Text("Places").caption().secondary();
                static const char *places[]={"Home","Documents","Downloads","Desktop"};
                for(unsigned i=0;i<4;i++) {
                    if(Button(places[i]).ghost().id(places[i]).clicked())go_to(place_path(i));
                }
            }

            VStack(.grow = 1, .align = Fill, .spacing = 0) {
                /* where we are, and the way back up */
                HStack(.spacing = 8, .align = Center, .padding = 8) {
                    if (Button("Up").ghost().id("up").clicked()) go_up();
                    Text(g_cwd).caption().secondary();
                    Spacer();
                    if(Button("New folder").ghost().clicked()) {g_new_folder=1;g_folder[0]=0;}
                }
                Divider();

                /* the listing */
                ScrollView(&g_scroll, 260) {
                    VStack(.align = Fill, .spacing = 1, .padding = 6) {
                        static const char *KEY[MAX_ENTRIES];
                        for (int i = 0; i < g_count; i++) {
                            KEY[i] = g_entries[i].name;
                            bool sel = (i == g_sel);
                            struct instance_handle row={0};bool activated=false;
                            HStack(.spacing = 8, .align = Center, .px = 8, .py = 5,
                                   .corner = t->radius_sm, .border=EmZero, .key = KEY[i],
                                   .background = sel ? t->accent_soft : (struct color){0,0,0,0}) {
                                row=ui_open();
                                Icon(g_entries[i].is_dir ? IconFolder : IconDoc)
                                    .color(g_entries[i].is_dir ? t->accent : t->text_secondary);
                                Text(g_entries[i].name);
                                activated=ui_control_interact(row);
                            }
                            /* Container keys preserve identity; they do not
                             * register the leaf-only Clicked(id) result. */
                            if(activated) {
                                if (g_entries[i].is_dir) {
                                    char full[256];
                                    join(full, sizeof full, g_entries[i].name);
                                    go_to(full);break;
                                } else {
                                    g_sel = i;
                                    memcpy(g_name,g_entries[i].name,sizeof g_name);
                                }
                            }
                        }
                        if (g_count == 0) EmptyState(IconFolder, "Nothing here", "");
                    }
                }

                if(g_error[0]) {Text(g_error).caption().secondary();em_flush();}
                Divider();
                /* the answer, and the two buttons that end this */
                HStack(.spacing = 8, .align = Center, .padding = 10) {
                    if (g_mode == EMFILE_SAVE) {
                        Text("Save as").caption().secondary();
                        if(TextField(g_name, sizeof g_name, "Filename").submitted() && g_name[0])choose(g_name);
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
        if(g_replace) {
            Overlay() {
                Dialog(.width=420,.spacing=14,.padding=20) {
                    Text("Replace this file?").heading();
                    Text(g_name).caption();
                    Text("The saved contents will replace the existing file.").caption().secondary();
                    HStack(.spacing=12) {
                        if(Button("Keep existing").ghost().clicked())g_replace=0;
                        Spacer();
                        if(Button("Replace").destructive().clicked())finish();
                    }
                }
            }
        }
        if(g_new_folder) {
            Overlay() {
                Dialog(.width=380,.spacing=14,.padding=20) {
                    Text("New folder").heading();
                    TextField(g_folder,sizeof g_folder,"Folder name");
                    HStack(.spacing=12) {
                        if(Button("Cancel folder").ghost().clicked())g_new_folder=0;
                        Spacer();
                        if(Button("Create").primary().clicked()) {
                            char full[256];join(full,sizeof full,g_folder);
                            if(!embk_file_valid_name(g_folder) || !full[0])
                                snprintf(g_error,sizeof g_error,"Enter a valid folder name");
                            else if(mkdir(full,0700))snprintf(g_error,sizeof g_error,"Cannot create this folder");
                            else {g_new_folder=0;g_error[0]=0;read_dir();}
                        }
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
        if (type != EMSVC_T_FILE_PANEL || len != sizeof q || q.mode>EMFILE_SAVE ||
            !memchr(q.title,0,sizeof q.title) || !memchr(q.start,0,sizeof q.start) ||
            !memchr(q.suggest,0,sizeof q.suggest)) {
            struct emfile_rep bad = { 0, -1, { 0 } };
            emsvc_reply(ch, type, &bad, sizeof bad);
            continue;
        }

        g_mode = (int)q.mode;
        snprintf(g_title, sizeof g_title, "%s", q.title);
        const char *home=getenv("HOME");snprintf(g_home,sizeof g_home,"%s",home&&home[0]?home:"/home");
        g_error[0]=0;g_cwd[0]=0;g_replace=g_new_folder=0;
        go_to(g_home);
        if(q.start[0])go_to(q.start);
        snprintf(g_name,sizeof g_name,"%s",q.suggest);
        em_set_key_hook(on_key);
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
