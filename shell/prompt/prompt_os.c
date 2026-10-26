/* ==========================================================================
 * prompt_os.c -- what Powerlevel10k's segments ask of the system. See
 * prompt_os.h.
 *
 * The directory segment's questions are answered from directory listings,
 * never by opening a name that may not be there (a missing file costs a line
 * in the kernel's log, and it asks about every folder marker in every part of
 * the path). The listings are kept for one prompt.
 * ========================================================================== */
#include "prompt/prompt_os.h"
#include "embk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KEPT 8                   /* listings: a path's parts and their parents */
#define LIST_CAP 8192            /* the most entries a listing grows to */

struct listing { char dir[256]; struct embk_dirent *e; int n; unsigned used; };
static struct listing g_kept[KEPT];
static unsigned g_clock;

void prompt_os_fresh(void) {
    for (int i = 0; i < KEPT; i++) { free(g_kept[i].e); g_kept[i].e = NULL; g_kept[i].dir[0] = 0; g_kept[i].n = 0; }
}

/* `dir`'s listing: kept, or read (the least recently used one making room).
 * NULL when it cannot be read. */
static const struct listing *listing(const char *dir) {
    if (!dir[0]) dir = "/";
    for (int i = 0; i < KEPT; i++)
        if (g_kept[i].e && !strcmp(g_kept[i].dir, dir)) { g_kept[i].used = ++g_clock; return &g_kept[i]; }
    struct listing *slot = &g_kept[0];
    for (int i = 1; i < KEPT; i++) if (!g_kept[i].e || g_kept[i].used < slot->used) slot = &g_kept[i];
    if (strlen(dir) >= sizeof slot->dir) return NULL;
    /* readdir fills at most what it is given: a full answer may have been cut */
    for (uint32_t cap = 256; cap <= LIST_CAP; cap *= 2) {
        struct embk_dirent *e = malloc(cap * sizeof *e);
        if (!e) return NULL;
        int64_t n = embk_readdir(dir, e, cap);
        if (n < 0) { free(e); return NULL; }
        if ((uint64_t)n < cap || cap == LIST_CAP) {
            free(slot->e);
            slot->e = e;
            slot->n = (int)n;
            slot->used = ++g_clock;
            snprintf(slot->dir, sizeof slot->dir, "%s", dir);
            return slot;
        }
        free(e);
    }
    return NULL;
}

static const struct embk_dirent *entry(const char *dir, const char *name) {
    const struct listing *l = listing(dir);
    for (int i = 0; l && i < l->n; i++) if (!strcmp(l->e[i].name, name)) return &l->e[i];
    return NULL;
}

static bool list(void *ctx, const char *dir, void (*each)(void *arg, const char *name, bool dir), void *arg) {
    (void)ctx;
    const struct listing *l = listing(dir);
    if (!l) return false;
    for (int i = 0; i < l->n; i++) {
        if (!strcmp(l->e[i].name, ".") || !strcmp(l->e[i].name, "..")) continue;
        each(arg, l->e[i].name, l->e[i].type == EMBK_DT_DIR);
    }
    return true;
}

/* The directory the shell is in, or one above it: there, so stat, not a
 * listing -- a session's namespace may grant /home/yves and not /home. */
static bool exists(void *ctx, const char *dir) {
    (void)ctx;
    struct embk_stat st;
    return embk_stat(dir, &st) >= 0;
}

/* Writable as the system says it (access(2)): EMBKFS has no permission bits
 * and nothing denies a write to what exists. */
static bool writable(void *ctx, const char *dir) {
    (void)ctx;
    return access(dir, W_OK) == 0;
}

const struct p10k_fs prompt_os_fs = { NULL, list, writable, exists };

/* A small file read whole (HEAD, a .git file); false when it cannot be. */
static bool slurp(const char *path, char *out, size_t cap) {
    int fd = (int)embk_open(path, 0, 0);
    if (fd < 0) return false;
    int64_t n = embk_read(fd, out, cap - 1);
    embk_close(fd);
    if (n < 0) return false;
    out[n] = 0;
    out[strcspn(out, "\r\n")] = 0;
    return true;
}

bool prompt_os_git(const char *pwd, struct p10k_git *g) {
    memset(g, 0, sizeof *g);
    char dir[256], gitdir[512], head[300];
    snprintf(dir, sizeof dir, "%s", pwd && pwd[0] ? pwd : "/");
    for (;;) {
        const struct embk_dirent *e = entry(dir, ".git");
        if (e) {
            snprintf(gitdir, sizeof gitdir, "%s/.git", strcmp(dir, "/") ? dir : "");
            if (e->type != EMBK_DT_DIR) {            /* a worktree's: "gitdir: <path>" */
                char line[256];
                if (!slurp(gitdir, line, sizeof line) || strncmp(line, "gitdir: ", 8)) return false;
                if (line[8] == '/') snprintf(gitdir, sizeof gitdir, "%s", line + 8);
                else snprintf(gitdir, sizeof gitdir, "%s/%s", strcmp(dir, "/") ? dir : "", line + 8);
            }
            char path[520];
            snprintf(path, sizeof path, "%s/HEAD", gitdir);
            if (!slurp(path, head, sizeof head)) return false;
            g->repo = true;
            g->loading = true;
            if (!strncmp(head, "ref: refs/heads/", 16)) snprintf(g->branch, sizeof g->branch, "%.*s", (int)sizeof g->branch - 1, head + 16);
            else snprintf(g->commit, sizeof g->commit, "%.40s", head);
            g->has_unstaged = 0;
            return true;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || !strcmp(dir, "/")) return false;
        if (slash == dir) dir[1] = 0;
        else *slash = 0;
    }
}
