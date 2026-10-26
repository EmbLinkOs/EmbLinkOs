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
#include "prompt/p10k_int.h"
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

/* --- the git status daemon -------------------------------------------------------- */

#define GS_ANSWERS 20            /* our ends of its pipes: clear of fd 3 (the shell's */
#define GS_ASKS 21               /* structured output) and eval_extern's 10 and 11 */
#define GS_KEEP 16               /* repositories whose last status is kept */

extern char **environ;

static int64_t g_daemon = -1;    /* its handle */
static bool g_absent;            /* no gitstatus.elf here: .git/HEAD will do */
static char g_flags[160];        /* what it was started with */
static unsigned g_serial;
static bool g_waiting;
static unsigned g_waiting_for;
static uint64_t g_asked_ms;
static char g_reply[8192];
static size_t g_reply_n;

/* The last status of each repository, by work directory (p10k's
 * _p9k__gitstatus_last), and whether it answers too slowly to wait for. */
struct known { char workdir[256]; struct p10k_git g; bool slow; unsigned used; };
static struct known g_known[GS_KEEP];
static unsigned g_clock;

static void daemon_stop(void) {
    if (g_daemon < 0) return;
    close(GS_ASKS);
    close(GS_ANSWERS);
    (void)embk_kill((int)g_daemon);
    (void)embk_wait((int)g_daemon);
    g_daemon = -1;
    g_waiting = false;
    g_reply_n = 0;
}

static bool daemon_start(const struct p10k *cfg) {
    char flags[160];
    snprintf(flags, sizeof flags, "-s %ld -u %ld -d %ld -c %ld -m %ld%s",
             p9k_int(cfg, "VCS_STAGED_MAX_NUM", 1), p9k_int(cfg, "VCS_UNSTAGED_MAX_NUM", 1),
             p9k_int(cfg, "VCS_UNTRACKED_MAX_NUM", 1), p9k_int(cfg, "VCS_CONFLICTED_MAX_NUM", 1),
             p9k_int(cfg, "VCS_MAX_INDEX_SIZE_DIRTY", -1), p9k_bool(cfg, "VCS_RECURSE_UNTRACKED_DIRS", false) ? " -e" : "");
    if (g_daemon >= 0 && !strcmp(flags, g_flags)) return true;
    daemon_stop();                                       /* another configuration: another daemon */
    if (g_absent) return false;
    if (!entry("/system/bin", "gitstatus.elf")) { g_absent = true; return false; }
    char words[160], *argv[16];
    int argc = 0;
    snprintf(words, sizeof words, "%s", flags);
    argv[argc++] = "gitstatus";
    for (char *w = strtok(words, " "); w && argc < 15; w = strtok(NULL, " ")) argv[argc++] = w;
    argv[argc] = NULL;
    int ask[2], answer[2];
    if (embk_pipe(ask)) return false;
    if (embk_pipe(answer)) { embk_close_handle(ask[0]); embk_close_handle(ask[1]); return false; }
    struct embk_spawn_file_action act[2];
    memset(act, 0, sizeof act);
    act[0].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ; act[0].target_fd = 0; act[0].src_obj_handle = ask[0];
    act[1].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ; act[1].target_fd = 1; act[1].src_obj_handle = answer[1];
    int64_t child = embk_spawn_env("/system/bin/gitstatus.elf", argv, environ, act, 2);
    embk_close_handle(ask[0]);
    embk_close_handle(answer[1]);
    bool ok = child >= 0 && embk_fd_install_obj(ask[1], GS_ASKS) == GS_ASKS &&
              embk_fd_install_obj(answer[0], GS_ANSWERS) == GS_ANSWERS;
    embk_close_handle(ask[1]);
    embk_close_handle(answer[0]);
    if (!ok) { if (child >= 0) { (void)embk_kill((int)child); (void)embk_wait((int)child); } g_absent = child < 0; return false; }
    g_daemon = child;
    snprintf(g_flags, sizeof g_flags, "%s", flags);
    return true;
}

static struct known *known_for(const char *workdir, bool add) {
    struct known *slot = &g_known[0];
    for (int i = 0; i < GS_KEEP; i++) {
        if (g_known[i].workdir[0] && !strcmp(g_known[i].workdir, workdir)) { g_known[i].used = ++g_clock; return &g_known[i]; }
        if (g_known[i].used < slot->used) slot = &g_known[i];
    }
    if (!add) return NULL;
    memset(slot, 0, sizeof *slot);
    snprintf(slot->workdir, sizeof slot->workdir, "%s", workdir);
    slot->used = ++g_clock;
    return slot;
}

/* p10k's _p9k_vcs_status_for_dir: the repository holding `dir`, as last known. */
static struct known *known_above(const char *dir) {
    char cur[256];
    snprintf(cur, sizeof cur, "%s", dir);
    for (;;) {
        struct known *k = known_for(cur, false);
        if (k) return k;
        char *slash = strrchr(cur, '/');
        if (!slash || !strcmp(cur, "/")) return NULL;
        if (slash == cur) cur[1] = 0; else *slash = 0;
    }
}

/* VCS_DISABLED_WORKDIR_PATTERN: a repository the prompt leaves out (lean's ~). */
static bool disabled(const struct p10k *cfg, const char *workdir) {
    const char *pat = p10k_get(cfg, "VCS_DISABLED_WORKDIR_PATTERN");
    if (!pat || !pat[0]) return false;
    const char *home = getenv("HOME");
    char full[512];
    if (pat[0] == '~' && home) snprintf(full, sizeof full, "%s%s", strcmp(home, "/") ? home : "", pat + 1);
    else snprintf(full, sizeof full, "%s", pat);
    return p9k_glob(full, strlen(full), workdir, strlen(workdir));
}

/* One answer's fields into `g` (gitstatusd's order); false: no repository. */
static bool parse(const struct p10k *cfg, char *reply, struct p10k_git *g, char *workdir, size_t cap) {
    char *f[32];
    int n = 0;
    for (char *p = reply; n < 32;) {
        f[n++] = p;
        char *us = strchr(p, '\x1f');
        if (!us) break;
        *us = 0;
        p = us + 1;
    }
    memset(g, 0, sizeof *g);
    if (n < 2 || strcmp(f[1], "1") || n < 29) return false;
    snprintf(workdir, cap, "%s", f[2]);
    if (disabled(cfg, workdir)) return false;
    g->repo = true;
    snprintf(g->commit, sizeof g->commit, "%s", f[3]);
    snprintf(g->branch, sizeof g->branch, "%s", f[4]);
    snprintf(g->remote_branch, sizeof g->remote_branch, "%s", f[5]);
    snprintf(g->remote_url, sizeof g->remote_url, "%s", f[7]);
    snprintf(g->action, sizeof g->action, "%s", f[8]);
    long index_size = atol(f[9]);
    g->staged = atoi(f[10]); g->unstaged = atoi(f[11]); g->conflicted = atoi(f[12]); g->untracked = atoi(f[13]);
    g->ahead = atoi(f[14]); g->behind = atoi(f[15]); g->stashes = atoi(f[16]);
    snprintf(g->tag, sizeof g->tag, "%s", f[17]);
    g->push_ahead = atoi(f[23]); g->push_behind = atoi(f[24]);
    snprintf(g->summary, sizeof g->summary, "%s", f[28]);
    long dirty = p9k_int(cfg, "VCS_MAX_INDEX_SIZE_DIRTY", -1);
    g->has_unstaged = dirty >= 0 && index_size > dirty ? -1 : g->unstaged > 0;   /* -1: too big to look */
    return true;
}

/* A whole answer to the awaited question on the pipe yet? Into `out` (its
 * fields still US-separated); answers to earlier questions are passed over. */
static bool reply_ready(char *out, size_t cap) {
    for (;;) {
        char *rs = memchr(g_reply, '\x1e', g_reply_n);
        if (rs) {
            size_t len = (size_t)(rs - g_reply), used = len + 1;
            char want[16];
            int wn = snprintf(want, sizeof want, "%u\x1f", g_waiting_for);
            bool mine = len >= (size_t)wn && !memcmp(g_reply, want, (size_t)wn);
            if (mine) {
                if (len >= cap) len = cap - 1;
                memcpy(out, g_reply, len);
                out[len] = 0;
            }
            memmove(g_reply, rs + 1, g_reply_n - used);   /* taken either way */
            g_reply_n -= used;
            if (mine) return true;
            continue;
        }
        int64_t avail = embk_fd_avail(GS_ANSWERS);
        if (avail <= 0 || g_reply_n == sizeof g_reply) return false;
        int64_t got = embk_read(GS_ANSWERS, g_reply + g_reply_n, sizeof g_reply - g_reply_n);
        if (got <= 0) { daemon_stop(); return false; }   /* it died: the next prompt starts another */
        g_reply_n += (size_t)got;
    }
}

/* An answer taken in: kept for the repository, the slow mark revised by
 * how long it took (p10k's _p9k_vcs_resume). */
static bool take(const struct p10k *cfg, char *reply, bool async, struct p10k_git *g) {
    char workdir[256];
    bool repo = parse(cfg, reply, g, workdir, sizeof workdir);
    g_waiting = false;
    if (!repo) return true;
    struct known *k = known_for(workdir, true);
    if (k) {
        k->g = *g;
        double took = (double)(embk_uptime_ms() - g_asked_ms) / 1000;
        double limit = p9k_float(cfg, "VCS_MAX_SYNC_LATENCY_SECONDS", 0.01);
        if (async && took > limit) k->slow = true;
        else if (took < 0.8 * limit) k->slow = false;
    }
    return true;
}

bool prompt_os_gitstatus(const struct p10k *cfg, const char *pwd, struct p10k_git *g) {
    memset(g, 0, sizeof *g);
    struct p10k_git head;
    if (!prompt_os_git(pwd, &head)) return false;        /* in no repository: nothing to ask */
    if (!daemon_start(cfg)) { *g = head; return true; }
    char req[300];
    int n = snprintf(req, sizeof req, "%u\x1f%s\x1f" "0\x1e", ++g_serial, pwd);
    if (n <= 0 || n >= (int)sizeof req || embk_write(GS_ASKS, req, (size_t)n) != n) { daemon_stop(); *g = head; return true; }
    g_waiting = true;
    g_waiting_for = g_serial;
    g_asked_ms = embk_uptime_ms();
    struct known *k = known_above(pwd);
    double limit = p9k_float(cfg, "VCS_MAX_SYNC_LATENCY_SECONDS", 0.01);
    uint64_t wait_ms = k && k->slow ? 0 : (uint64_t)(limit * 1000 + 0.5);
    for (uint64_t start = embk_uptime_ms();;) {
        static char reply[sizeof g_reply];
        if (reply_ready(reply, sizeof reply)) return take(cfg, reply, false, g) && g->repo;
        if (g_daemon < 0) { *g = head; return true; }
        if (embk_uptime_ms() - start >= wait_ms) break;
        embk_sleep_ms(1);
    }
    /* not yet: the last known status, loading; nothing known, VCS_LOADING_TEXT */
    if (k) *g = k->g;
    else { memset(g, 0, sizeof *g); g->repo = true; }
    g->loading = true;
    return true;
}

int prompt_os_git_waiting(void) { return g_waiting && g_daemon >= 0 ? GS_ANSWERS : -1; }

bool prompt_os_git_answer(const struct p10k *cfg, struct p10k_git *g) {
    static char reply[sizeof g_reply];
    if (!g_waiting || !reply_ready(reply, sizeof reply)) return false;
    take(cfg, reply, true, g);
    return true;
}
