/* user/apps/files/files.c -- the file manager.
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
#define TRASH_INDEX_NAME ".trash-index"   /* the Trash's own bookkeeping */
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

/* Right-click target and the three destructive-ish verbs. Everything that can
 * lose data goes through a dialog: not a habit-forming "are you sure" on every
 * action, but one on the ONE action that cannot be undone. */
static bool  g_menu_open = false;
static float g_menu_x, g_menu_y;
static int   g_menu_i = -1;          /* entry the menu was opened on */
static bool  g_confirm_del = false;
static bool  g_confirm_empty = false;   /* Empty Trash asks once, inline */  /* the delete sheet             */
static bool  g_renaming = false;     /* the rename sheet             */
static char  g_rename_to[NAME_LEN] = "";
static char  g_clip_path[600] = "";  /* the "copied" file            */
static char  g_clip_name[NAME_LEN] = "";

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
        /* The Trash's own record of where things came from. Filtered here
         * rather than hidden by a dotfile rule, because this shell does not
         * have one -- .vellum is listed like anything else -- and inventing
         * one to sweep this under would hide files the user put there. */
        if (!strcmp(de->d_name, TRASH_INDEX_NAME)) continue;
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

/* --- the Trash ----------------------------------------------------------
 *
 * The sidebar has had a Trash folder for a long time and nothing ever went
 * into it: Delete called unlink, and the confirmation sheet said so in as many
 * words -- "there is no Trash yet, so the file is gone for good". Honest, and
 * still the wrong behaviour for the verb everyone reaches for most carelessly.
 *
 * A trash is a RENAME, not a copy: same filesystem, so it is instant whatever
 * the file's size, and a machine that stalls for a minute when you delete a
 * video teaches you not to delete things.
 *
 * WHERE IT CAME FROM is recorded beside it, one line per item in
 * $HOME/.trash-index ("<trashed name>\t<original full path>"). That file is
 * what makes Put Back a click instead of an act of memory, and it is plain
 * text on purpose: if it is ever lost, the trash is still a folder full of
 * your files, which is the failure mode to prefer.
 */
static void trash_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/Trash", home && home[0] ? home : "/");
}
static int in_trash(void) {
    char t[560]; trash_dir(t, sizeof t);
    return strcmp(g_cwd, t) == 0;
}
/* THE INDEX LIVES IN THE TRASH, not in your home directory.
 *
 * It was $HOME/.trash-index, and Files does not hide dotfiles -- so the first
 * thing anybody deleted put a permanent new item in their home folder. The
 * bookkeeping belongs with the thing it describes, and it is filtered out of
 * the listing below: the Trash's own record is not an item IN the Trash. */
static void trash_index_path(char *out, size_t cap) {
    char td[560]; trash_dir(td, sizeof td);
    snprintf(out, cap, "%s/%s", td, TRASH_INDEX_NAME);
}
/* Remember (or forget, orig == NULL) where one trashed item came from. The
 * whole file is rewritten: it is a handful of lines, and a rewrite cannot
 * leave a half-updated one behind. */
static void trash_index_set(const char *name, const char *orig) {
    { char td[560]; trash_dir(td, sizeof td); embk_mkdir(td); }
    char ip[600]; trash_index_path(ip, sizeof ip);
    static char body[8192];
    int n = 0;
    FILE *f = fopen(ip, "r");
    if (f) { n = (int)fread(body, 1, sizeof body - 1, f); fclose(f); }
    if (n < 0) n = 0;
    body[n] = 0;

    static char out[8192];
    int o = 0;
    /* copy every line that is not about `name` */
    for (char *p = body; *p; ) {
        char *line = p;
        while (*p && *p != '\n') p++;
        int len = (int)(p - line);
        if (*p) p++;
        if (len <= 0) continue;
        int skip = (strncmp(line, name, strlen(name)) == 0 && line[strlen(name)] == '\t');
        if (!skip && o + len + 1 < (int)sizeof out) {
            memcpy(out + o, line, (size_t)len); o += len; out[o++] = '\n';
        }
    }
    if (orig) o += snprintf(out + o, sizeof out - (size_t)o, "%s\t%s\n", name, orig);
    f = fopen(ip, "w");
    if (f) { fwrite(out, 1, (size_t)o, f); fclose(f); }
}
/* Where `name` came from, or 0. */
static const char *trash_index_get(const char *name) {
    static char found[600];
    char ip[600]; trash_index_path(ip, sizeof ip);
    static char body[8192];
    FILE *f = fopen(ip, "r");
    if (!f) return 0;
    int n = (int)fread(body, 1, sizeof body - 1, f);
    fclose(f);
    if (n <= 0) return 0;
    body[n] = 0;
    size_t nl = strlen(name);
    for (char *p = body; *p; ) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        if (strncmp(line, name, nl) == 0 && line[nl] == '\t') {
            snprintf(found, sizeof found, "%s", line + nl + 1);
            return found;
        }
    }
    return 0;
}

/* Delete. Inside the Trash it is permanent -- that is what the Trash is for --
 * and everywhere else it is a move into it.
 *
 * Directories go through rmdir when the delete IS permanent, which refuses a
 * non-empty one; that refusal is reported rather than worked around, because
 * recursive delete is a different and much more dangerous verb and should have
 * to be asked for. A directory moved to the Trash keeps its contents, which is
 * the one way a folder full of things can be deleted safely. */
static void do_delete(int i) {
    if (i < 0 || i >= g_count) return;
    struct entry *e = &g_entries[i];
    char full[600];
    join(full, sizeof full, e->name);

    if (in_trash()) {
        int rc = e->is_dir ? embk_rmdir(full) : embk_unlink(full);
        if (rc >= 0) { trash_index_set(e->name, 0);
                       snprintf(g_status, sizeof g_status, "Deleted %s for good", e->name); }
        else if (e->is_dir)
            snprintf(g_status, sizeof g_status, "%s is not empty", e->name);
        else snprintf(g_status, sizeof g_status, "Could not delete %s", e->name);
        g_dirty = true;
        return;
    }

    char td[560]; trash_dir(td, sizeof td);
    embk_mkdir(td);                             /* harmless if it is already there */

    /* A name already in the Trash is not a reason to overwrite it: two files
     * called notes.txt deleted a week apart are two different files. */
    char dst[700], leaf[280];
    snprintf(leaf, sizeof leaf, "%s", e->name);
    snprintf(dst, sizeof dst, "%s/%s", td, leaf);
    for (int k = 2; k < 1000; k++) {
        int probe = (int)embk_open(dst, EMBK_O_RDONLY, 0);
        if (probe < 0) break;
        embk_close(probe);
        snprintf(leaf, sizeof leaf, "%s %d", e->name, k);
        snprintf(dst, sizeof dst, "%s/%s", td, leaf);
    }
    int rc = rename(full, dst);
    { char b[256]; snprintf(b, sizeof b, "files: trash %s -> %s : rc=%d\n",
                            full, dst, rc); embk_puts(1, b); }
    if (rc == 0) {
        trash_index_set(leaf, full);
        snprintf(g_status, sizeof g_status, "Moved %s to the Trash", e->name);
    } else {
        snprintf(g_status, sizeof g_status, "Could not move %s to the Trash", e->name);
    }
    g_dirty = true;
}

/* Put one item back where it came from. */
static void do_put_back(int i) {
    if (i < 0 || i >= g_count) return;
    struct entry *e = &g_entries[i];
    const char *orig = trash_index_get(e->name);
    if (!orig) {
        snprintf(g_status, sizeof g_status,
                 "Where %s came from was not recorded", e->name);
        return;
    }
    char from[600];
    join(from, sizeof from, e->name);
    int probe = (int)embk_open(orig, EMBK_O_RDONLY, 0);
    if (probe >= 0) {
        embk_close(probe);
        snprintf(g_status, sizeof g_status, "Something is already there");
        return;
    }
    if (rename(from, orig) == 0) {
        trash_index_set(e->name, 0);
        snprintf(g_status, sizeof g_status, "Put %s back", e->name);
    } else {
        snprintf(g_status, sizeof g_status, "Could not put %s back", e->name);
    }
    g_dirty = true;
}

/* Everything in the Trash, gone. Files only: a folder in there is left alone
 * and reported, for the same reason a recursive delete has to be asked for. */
static void do_empty_trash(void) {
    int gone = 0, kept = 0;
    for (int i = 0; i < g_count; i++) {
        struct entry *e = &g_entries[i];
        char full[600];
        join(full, sizeof full, e->name);
        int rc = e->is_dir ? embk_rmdir(full) : embk_unlink(full);
        if (rc >= 0) { trash_index_set(e->name, 0); gone++; }
        else kept++;
    }
    if (kept) snprintf(g_status, sizeof g_status,
                       "Emptied %d; %d could not be removed (a folder is not empty)",
                       gone, kept);
    else snprintf(g_status, sizeof g_status, "Emptied the Trash (%d item%s)",
                  gone, gone == 1 ? "" : "s");
    g_dirty = true;
}

static void do_rename(int i) {
    if (i < 0 || i >= g_count || !g_rename_to[0]) return;
    char from[600], to[600];
    join(from, sizeof from, g_entries[i].name);
    join(to,   sizeof to,   g_rename_to);
    if (rename(from, to) == 0) snprintf(g_status, sizeof g_status, "Renamed to %s", g_rename_to);
    else snprintf(g_status, sizeof g_status, "Could not rename (does %s exist?)", g_rename_to);
    g_dirty = true;
}

/* Copy is two verbs: remembering a source, then writing it somewhere. Only
 * files -- copying a directory means walking it, and a half-copied tree is a
 * worse outcome than a refusal. */
static void do_paste(void) {
    if (!g_clip_path[0]) return;
    char dest[600];
    join(dest, sizeof dest, g_clip_name);
    int in = (int)embk_open(g_clip_path, EMBK_O_RDONLY, 0);
    if (in < 0) { snprintf(g_status, sizeof g_status, "Cannot read %s", g_clip_name); return; }
    int out = (int)embk_open(dest, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0644);
    if (out < 0) { embk_close(in);
        snprintf(g_status, sizeof g_status, "Cannot write here"); return; }
    static char buf[8192];
    int64_t n, total = 0; int ok = 1;
    while ((n = embk_read(in, buf, sizeof buf)) > 0) {
        if (embk_write(out, buf, (size_t)n) != n) { ok = 0; break; }
        total += n;
    }
    embk_close(in); embk_close(out);
    snprintf(g_status, sizeof g_status, ok ? "Copied %s" : "Copy of %s failed", g_clip_name);
    g_dirty = true;
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

/* The right-click is consumed ONCE per frame and then hit-tested against each
 * row's rect. em_right_clicked is read-and-clear -- asking it per row would let
 * the first row swallow the event and every other row would never see one. */
static bool  g_rc_armed = false;
static float g_rc_x, g_rc_y;

static void rc_begin(void) {
    float x, y;
    if (RightClicked(&x, &y)) { g_rc_armed = true; g_rc_x = x; g_rc_y = y; }
}

/* Called from inside a row's container, where ui_open_rect knows its box. */
/* DRAGGING A FILE OUT OF THIS WINDOW.
 *
 * The press captures the pointer (the compositor routes every later motion to
 * the window it landed on), so this keeps getting told where the cursor is the
 * whole way across the screen -- which is what makes a cross-application drag
 * possible at all, and also why the DROP has to be resolved by the compositor
 * rather than by whoever is underneath.
 *
 * A THRESHOLD, because a click is a press that did not move. Without one, every
 * single click on a file would announce a drag and then cancel it, and the
 * distinction between opening a file and picking it up would be a race. */
#define DRAG_SLOP 6.0f
static int   g_drag_i = -1;        /* the entry being carried, or -1 */
static int   g_drag_live;          /* embk_drag_begin has been called */
static float g_drag_x0, g_drag_y0;
/* OPENING HAPPENS ON RELEASE, not on press, and only when the press did not
 * turn into a drag. The toolkit fires .clicked() on the PRESS edge, which is
 * right for a button and wrong for a file: it meant you could not pick a file
 * up without also opening it, and the drag arrived at its destination behind a
 * window that had just launched. A click is a press that did not move. */
static int   g_open_i = -1;

static void drag_claim(int i) {
    if (!ui_is_active()) {
        /* The button is up. If we were carrying something the compositor has
         * already worked out where it landed and all that is left is to put it
         * down; if we were not, this press never moved and is a click. */
        if (g_drag_i == i) {
            /* NOT cancelled here. The compositor ends the drag itself at the
             * release edge -- and cancelling also WIPES THE PAYLOAD, which the
             * target has not read yet: it is woken by the release and polls on
             * its next frame, by which time this had already thrown the path
             * away. The drop arrived empty and looked like no drop at all.
             *
             * What is left behind is a payload nobody is carrying, which is
             * exactly what the clipboard is too. */
            if (g_drag_live) g_drag_live = 0;
            else             g_open_i = i;
            g_drag_i = -1;
        }
        return;
    }
    float px, py;
    ui_pointer_pos(&px, &py);
    if (g_drag_i != i) { g_drag_i = i; g_drag_x0 = px; g_drag_y0 = py; g_drag_live = 0; }
    if (g_drag_live) return;

    float dx = px - g_drag_x0, dy = py - g_drag_y0;
    if (dx * dx + dy * dy < DRAG_SLOP * DRAG_SLOP) return;

    /* Far enough to mean it. The payload is the full path, because a name on
     * its own is only meaningful to the window it came from. */
    char full[512];
    join(full, sizeof full, g_entries[i].name);
    embk_drag_begin("path", full, (unsigned)strlen(full));
    g_drag_live = 1;
}

static void rc_claim(int i) {
    if (!g_rc_armed) return;
    float x, y, w, h;
    if (!ui_open_rect(&x, &y, &w, &h) || w <= 1 || h <= 1) return;
    if (g_rc_x < x || g_rc_x >= x + w || g_rc_y < y || g_rc_y >= y + h) return;
    g_rc_armed = false;                       /* claimed */
    g_menu_i = i; g_menu_x = g_rc_x; g_menu_y = g_rc_y; g_menu_open = true;
}



/* Ghost buttons paint their label in the ACCENT, which is right for an action
 * and wrong for a name. A window where every filename and every place is
 * accent-blue reads as a page of hyperlinks: the accent stops meaning
 * "this one" when everything wears it. So names are text-coloured, and the
 * accent is spent on exactly one thing -- the place you are actually in. */
/* A Finder sidebar row: small icon, caption-sized label, tight rhythm. The
 * first version was twice this tall with body-sized text, which is an Android
 * list -- comfortable for a thumb, wasteful for a pointer. On a desktop the
 * sidebar is a reference you glance at, so it should be dense enough to take
 * in at once. */
static void sidebar_item(int icon, const char *label, const char *path) {
    const struct ui_theme *t = ui_theme();
    bool here = path && !strcmp(path, g_cwd);
    HStack(.spacing = 7, .align = Center, .px = 7, .py = 1, .corner = 5, .grow = 1,
           .background = here ? (Color){ .r=.24f, .g=.26f, .b=.34f, .a=1.f }
                              : (Color){ 0, 0, 0, 0 }) {
        Icon(icon).caption().color(here ? t->accent : t->text_secondary);
        /* .py(1): the row's height was never the HStack's to give. A ghost
         * button carries the toolkit's standard control padding, which is
         * sized for a tappable control, and THAT is what kept these rows
         * Android-tall no matter what the container asked for. */
        if (Button(label).ghost().font(Caption).py(1)
                         .color(here ? t->text : t->text_secondary)
                         .grow().leading().clicked()) navigate_to(path);
    }
}

static void grid_cell(int i) {
    struct entry *e = &g_entries[i];
    VStack(.spacing = 2, .width = 92, .align = Center) {
        rc_claim(i);
        drag_claim(i);
        /* The press is not acted on here -- drag_claim decides on RELEASE
         * whether it was a click or the start of a drag. Both of these still
         * have to be emitted to draw and to take the press. */
        if (e->is_dir) {
            if (ImageButtonKey("/system/images/file.eic", 44, e)) { /* see drag_claim */ }
        } else {
            if (IconButton(icon_for(e)).frame(44, 44).font(Title).clicked()) { }
        }
        if (Button(e->name).ghost().font(Caption).py(1).color(ui_theme()->text).width(90).clicked()) { }
    }
}

static void list_row(int i) {
    struct entry *e = &g_entries[i];
    char sz[24];
    bool sel = (g_sel == i);
    HStack(.spacing = 10, .align = Center, .px = 8, .py = 0, .corner = 4, .grow = 1,
           .background = sel ? (Color){ .r=.24f, .g=.26f, .b=.34f, .a=1.f }
                             : (Color){ 0, 0, 0, 0 }) {
        rc_claim(i);
        Icon(icon_for(e)).caption().secondary();
        if (Button(e->name).ghost().font(Caption).py(1).color(ui_theme()->text)
                           .grow().leading().clicked()) {
            g_sel = i;
            if (e->is_dir) enter_dir(e->name); else open_file(e->name);
        }
        /* The way IN to the per-file verbs. Right-click arms the same menu,
         * but a destructive action reachable only by a gesture with no visible
         * affordance is a feature most people never find -- and right-click
         * events are not currently reaching applications at all (logged in
         * docs/TODO.md), so this is also the only path that works today. */
        if (Button("...").ghost().font(Caption).py(1).px(6)
                .color(ui_theme()->text_secondary).id(e->name).clicked()) {
            g_menu_i = i; g_menu_open = true;
            g_menu_x = 260.0f; g_menu_y = 90.0f + (float)i * 22.0f;
        }
        Text(kind_of(e)).caption().tertiary();
        /* Sizes trail so the digits line up down the column and can be compared
         * without reading every one of them. */
        Text(e->is_dir ? "--" : human_size(e->size, sz, sizeof sz)).caption().tertiary();
    }
}

/* ---- the window -------------------------------------------------------- */

static void app(void) {
    /* A CLICK THAT NEVER BECAME A DRAG, acted on at the top of the frame after
     * the release that produced it. Deferred rather than done in drag_claim
     * because opening tears down and rebuilds this view, and doing that from
     * inside it is how a tree gets rebuilt underneath the code walking it. */
    if (g_open_i >= 0) {
        int i = g_open_i; g_open_i = -1;
        if (i < g_count) {
            if (g_entries[i].is_dir) enter_dir(g_entries[i].name);
            else                     open_file(g_entries[i].name);
        }
    }

    if (!g_initialized) {
        const char *start = getenv("FILES_PATH");
        if (!start || start[0] != '/') start = getenv("HOME");
        if (start && start[0] == '/') snprintf(g_cwd, sizeof g_cwd, "%s", start);
        g_initialized = true;
    }
    if (g_dirty) { read_dir(); g_dirty = false; }
    build_visible();
    rc_begin();

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
                HStack(.spacing = 6, .align = Center, .px = 10, .py = 5) {
                    if (IconButton(IconChevronL).clicked()) go_back();
                    if (IconButton(IconChevronR).clicked()) go_forward();
                    if (IconButton(IconChevronU).clicked()) go_up();
                    Card(.padding = 5, .grow = 1, .corner = 6,
                         .background = { .r=.12f, .g=.125f, .b=.145f, .a=1.f }) {
                        Text(g_cwd).caption();
                    }
                    if (g_naming) {
                        TextField(g_newname, sizeof g_newname, "Folder name");
                        if (Button("Create").primary().clicked()) create_folder();
                        if (Button("Cancel").ghost().clicked()) { g_naming = false; g_newname[0] = 0; }
                    } else if (in_trash()) {
                        /* IN THE TRASH the folder verbs are the wrong ones:
                         * nobody makes a new folder in there. Empty it is the
                         * only thing this place is for, and it only appears
                         * when there is something to empty -- a button that
                         * would do nothing is a button you learn to ignore. */
                        if (g_count > 0) {
                            if (g_confirm_empty) {
                                Text("Delete everything?").caption().secondary();
                                if (Button("Empty Trash").destructive().font(Caption).clicked()) {
                                    do_empty_trash(); g_confirm_empty = false;
                                }
                                if (Button("Cancel").ghost().font(Caption)
                                        .color(ui_theme()->text).clicked()) g_confirm_empty = false;
                            } else if (Button("Empty Trash").ghost().font(Caption)
                                           .color(ui_theme()->text).clicked()) {
                                g_confirm_empty = true;
                            }
                        }
                    } else {
                        if (Button("New Folder").ghost().font(Caption)
                                .color(ui_theme()->text).clicked()) {
                            g_naming = true; g_newname[0] = 0;
                        }
                        /* Paste acts on the FOLDER you are in, so it belongs on
                         * the toolbar; the per-item verbs live in the context
                         * menu. It only appears when there is something to
                         * paste -- a permanently greyed control is clutter that
                         * teaches you to ignore that corner of the window. */
                        if (g_clip_path[0]) {
                            char lbl[NAME_LEN + 16];
                            snprintf(lbl, sizeof lbl, "Paste %s", g_clip_name);
                            if (Button(lbl).ghost().font(Caption)
                                    .color(ui_theme()->accent).clicked()) do_paste();
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
                    VStack(.spacing = g_view ? 0 : 10, .align = Fill, .padding = 10) {
                        /* The placeholder and the listing occupy the same
                         * slot here. That used to mean the second inherited
                         * the first's padding -- the reconciler matched them
                         * because every container is one instance kind -- and
                         * the listing drew 32 px down and 24 px right of where
                         * it belonged. Fixed in the toolkit rather than here:
                         * a container's stacking axis is part of its identity
                         * now (ui/declare/instance.h). */
                        if (g_vis_n == 0) {
                            VStack(.align = Fill) {
                                if (g_query[0])
                                    EmptyState(IconSearch, "No matches",
                                               "Nothing in this folder matches what you typed.");
                                else
                                    EmptyState(IconFolder, "Empty folder",
                                               "There is nothing here yet.");
                            }
                        } else if (g_view == 1) {
                            VStack(.align = Fill) {
                                for (int k = 0; k < g_vis_n; k++) list_row(g_vis[k]);
                            }
                        } else {
                            /* against the PANE's width (window less sidebar
                             * and padding), not the window's -- the last
                             * column was overflowing and getting clipped. */
                            int cols = ((int)em_viewport_width() - 178 - 44) / 100;
                            if (cols < 2) cols = 2;
                            if (cols > 8) cols = 8;
                            VStack(.align = Fill, .spacing = 10) {
                                for (int base = 0; base < g_vis_n; base += cols) {
                                    HStack(.spacing = 8, .align = Leading) {
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
                }

                /* The verbs live in a context menu rather than a toolbar:
                 * they act on ONE item, and a toolbar button cannot say which.
                 * Right-click is also where a Mac user reaches for them. */
                /* Emitted INSIDE the content pane, deliberately.
                 *
                 * em_context_menu_ offsets its panel from its PARENT's origin
                 * while em_right_clicked reports WINDOW-local coordinates, so
                 * the menu opens down and right of the pointer by the sidebar
                 * width and the title-bar height. Moving it to the window's top
                 * level fixes the anchor and BREAKS the menu: the dismiss scrim
                 * is then viewport-sized over the press point and swallows the
                 * very click that opened it. A menu in the wrong place beats a
                 * menu that never appears; the real fix is for the anchor to be
                 * expressed in window coordinates. See docs/TODO.md.
                 *
                 * Right-click on empty space is about the FOLDER. Finder does
                 * this, and it is where New Folder and Paste actually belong --
                 * they were only on the toolbar because there was nowhere else
                 * to put them. Any click no row claimed lands here. */
                if (g_rc_armed) {
                    g_rc_armed = false;
                    g_menu_i = -1; g_menu_x = g_rc_x; g_menu_y = g_rc_y; g_menu_open = true;
                }
                if (g_menu_open && g_menu_i < 0) {
                    ContextMenu(&g_menu_open, g_menu_x, g_menu_y) {
                        if (MenuItem("New Folder")) { g_naming = true; g_newname[0] = 0; }
                        if (g_clip_path[0]) {
                            char lbl[NAME_LEN + 16];
                            snprintf(lbl, sizeof lbl, "Paste %s", g_clip_name);
                            if (MenuItem(lbl)) do_paste();
                        }
                        MenuSeparator();
                        if (MenuItem("Refresh")) g_dirty = true;
                    }
                }
                if (g_menu_open && g_menu_i >= 0 && g_menu_i < g_count) {
                    struct entry *e = &g_entries[g_menu_i];
                    ContextMenu(&g_menu_open, g_menu_x, g_menu_y) {
                        if (MenuItem(e->is_dir ? "Open" : "Open in Editor")) {
                            if (e->is_dir) enter_dir(e->name); else open_file(e->name);
                        }
                        MenuSeparator();
                        if (MenuItem("Copy")) {
                            if (e->is_dir) snprintf(g_status, sizeof g_status,
                                                    "Copying folders is not supported yet");
                            else {
                                join(g_clip_path, sizeof g_clip_path, e->name);
                                snprintf(g_clip_name, sizeof g_clip_name, "%s", e->name);
                                snprintf(g_status, sizeof g_status, "Copied %s", e->name);
                            }
                        }
                        if (MenuItem("Rename...")) {
                            snprintf(g_rename_to, sizeof g_rename_to, "%s", e->name);
                            g_renaming = true;
                        }
                        MenuSeparator();
                        if (in_trash() && MenuItem("Put Back")) do_put_back(g_menu_i);
                        if (MenuItem(in_trash() ? "Delete for Good..." : "Move to Trash..."))
                            g_confirm_del = true;
                    }
                }

                /* DELETE asks first, and the question names the thing and says
                 * what is permanent about it. The confirming button is the
                 * destructive-styled one and it is NOT the default position --
                 * a sheet you can dismiss by reflex should dismiss SAFELY. */
                if (g_confirm_del && g_menu_i >= 0 && g_menu_i < g_count) {
                    struct entry *e = &g_entries[g_menu_i];
                    Overlay() {
                        Dialog(.width = 380) {
                            /* THE SHEET SAYS WHICH DELETE THIS IS. Inside the
                             * Trash it is permanent and asks like it; anywhere
                             * else it is a move, which is recoverable and does
                             * not deserve the same alarm. Asking the same
                             * question for both teaches people to click
                             * through it. */
                            bool perm = in_trash();
                            Text(perm ? (e->is_dir ? "Delete this folder for good?"
                                                   : "Delete this file for good?")
                                      : (e->is_dir ? "Move this folder to the Trash?"
                                                   : "Move this file to the Trash?")).title();
                            Text(e->name).body().secondary();
                            Text(perm ? "It is in the Trash, so this cannot be undone."
                                      : "You can put it back from the Trash.")
                                .caption().tertiary();
                            HStack(.spacing = 8, .align = Center, .pt = 12) {
                                Spacer();
                                if (Button("Cancel").ghost().color(ui_theme()->text).clicked())
                                    g_confirm_del = false;
                                if (Button(perm ? "Delete" : "Move to Trash").destructive().clicked()) {
                                    do_delete(g_menu_i); g_confirm_del = false; g_menu_i = -1;
                                }
                            }
                        }
                    }
                    if (OverlayDismissed()) g_confirm_del = false;
                }

                /* RENAME does not ask -- it is reversible by renaming back, and
                 * a confirmation on a reversible action is just a keystroke tax. */
                if (g_renaming && g_menu_i >= 0 && g_menu_i < g_count) {
                    Overlay() {
                        Dialog(.width = 380) {
                            Text("Rename").title();
                            TextField(g_rename_to, sizeof g_rename_to, "New name");
                            HStack(.spacing = 8, .align = Center, .pt = 12) {
                                Spacer();
                                if (Button("Cancel").ghost().color(ui_theme()->text).clicked())
                                    g_renaming = false;
                                if (Button("Rename").primary().clicked()) {
                                    do_rename(g_menu_i); g_renaming = false; g_menu_i = -1;
                                }
                            }
                        }
                    }
                    if (OverlayDismissed()) g_renaming = false;
                }
                /* status line: what am I looking at */
                Divider();
                HStack(.spacing = 10, .align = Center, .px = 12, .py = 3) {
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
