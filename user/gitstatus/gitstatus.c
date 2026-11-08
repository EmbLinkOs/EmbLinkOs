/* ==========================================================================
 * gitstatus -- the git status of directories, for the prompt.
 *
 * Speaks gitstatusd's protocol (Powerlevel10k's daemon), so the shell asks
 * it as p10k asks gitstatusd: requests on stdin, `id US dir US no-diff RS`;
 * for each an answer on stdout, `id US 1 US` and 25 fields more, or
 * `id US 0` when the directory is in no repository. One runs per shell for
 * as long as the shell keeps its stdin open, and keeps what it learned of
 * each repository (gs_status.c) between questions.
 *
 *   gitstatus [-s N] [-u N] [-c N] [-d N] [-m N] [-e] [-U] [-W] [-D] [-z N]
 *
 * as gitstatusd: at most N staged, unstaged, conflicted, untracked (-1: all;
 * 1 by default), -m: unstaged and untracked as 0 past N index entries, -e:
 * every untracked file rather than untracked directories as one, -U/-W/-D:
 * count what status.showUntrackedFiles, bash.showUntrackedFiles and
 * bash.showDirtyState would hide, -z: the summary's bytes. gitstatusd's -t,
 * -v, -r, -p, -l and -G are taken and have no effect here.
 * ========================================================================== */
#include "gs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define US '\x1f'
#define RS '\x1e'
#define OPEN 8                 /* repositories kept open */

static struct gs_repo *g_open[OPEN];
static unsigned g_used[OPEN], g_clock;

/* The repository `dir` is in: one kept, or opened (closing the least used). */
static struct gs_repo *repo_for(const char *dir) {
    struct gs_repo *r = gs_repo_open(dir);
    if (!r) return NULL;
    for (int i = 0; i < OPEN; i++)
        if (g_open[i] && !strcmp(gs_repo_gitdir(g_open[i]), gs_repo_gitdir(r)) && !strcmp(g_open[i]->workdir, r->workdir)) {
            gs_repo_close(r);
            g_used[i] = ++g_clock;
            return g_open[i];
        }
    int slot = 0;
    for (int i = 1; i < OPEN; i++) if (!g_open[i] || g_used[i] < g_used[slot]) slot = i;
    gs_repo_close(g_open[slot]);
    g_open[slot] = r;
    g_used[slot] = ++g_clock;
    return r;
}

static void field(const char *s) { putchar(US); fputs(s, stdout); }
static void num(long n) { printf("%c%ld", US, n); }

static void answer(const char *id, const char *dir, bool no_diff, const struct gs_options *o) {
    fputs(id, stdout);
    struct gs_repo *r = repo_for(dir);
    struct gs_status s;
    if (!r || !gs_repo_status(r, o, no_diff, &s)) { printf("%c0%c", US, RS); fflush(stdout); return; }
    putchar(US); putchar('1');
    field(s.workdir); field(s.commit); field(s.branch); field(s.upstream); field(s.remote); field(s.remote_url);
    field(s.action); num(s.index_size); num(s.staged); num(s.unstaged); num(s.conflicted); num(s.untracked);
    num(s.ahead); num(s.behind); num(s.stashes); field(s.tag); num(s.unstaged_deleted); num(s.staged_new);
    num(s.staged_deleted); field(s.push_remote); field(s.push_remote_url); num(s.push_ahead); num(s.push_behind);
    num(s.skip_worktree); num(s.assume_unchanged); field(s.encoding); field(s.summary);
    putchar(RS);
    fflush(stdout);
}

static bool take_long(int argc, char **argv, int *i, long *out) {
    if (*i + 1 >= argc) return false;
    char *end;
    *out = strtol(argv[++*i], &end, 10);
    return *end == 0;
}

int main(int argc, char **argv) {
    struct gs_options o = { 1, 1, 1, 1, -1, false, false, false, false, 256, true };
#ifdef EMBLINK_TARGET
    o.filemode = false;                                 /* EMBKFS keeps no exec bit to compare */
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool ok = true;
        if (!strcmp(a, "-s")) ok = take_long(argc, argv, &i, &o.max_staged);
        else if (!strcmp(a, "-u")) ok = take_long(argc, argv, &i, &o.max_unstaged);
        else if (!strcmp(a, "-c")) ok = take_long(argc, argv, &i, &o.max_conflicted);
        else if (!strcmp(a, "-d")) ok = take_long(argc, argv, &i, &o.max_untracked);
        else if (!strcmp(a, "-m")) ok = take_long(argc, argv, &i, &o.dirty_max_index_size);
        else if (!strcmp(a, "-z")) ok = take_long(argc, argv, &i, &o.max_summary);
        else if (!strcmp(a, "-e")) o.recurse_untracked_dirs = true;
        else if (!strcmp(a, "-U")) o.ignore_status_show_untracked = true;
        else if (!strcmp(a, "-W")) o.ignore_bash_show_untracked = true;
        else if (!strcmp(a, "-D")) o.ignore_bash_show_dirty_state = true;
        else if (!strcmp(a, "-t") || !strcmp(a, "-v") || !strcmp(a, "-r") || !strcmp(a, "-p") || !strcmp(a, "-l") ||
                 !strcmp(a, "-G")) { if (i + 1 < argc) i++; }
        else if (!strcmp(a, "-V")) { puts("v1.5.4-emblink"); return 0; }
        else ok = false;
        if (!ok) { fprintf(stderr, "gitstatus: bad option %s\n", a); return 2; }
    }
    struct gs_buf req = { 0 };
    for (int ch; (ch = getchar()) != EOF;) {
        if (ch != RS) { char c = (char)ch; gs_buf_add(&req, &c, 1); continue; }
        gs_buf_add(&req, "", 0);
        char *id = (char *)req.p, *dir = strchr(id, US), *flag = NULL;
        if (dir) { *dir++ = 0; flag = strchr(dir, US); if (flag) *flag++ = 0; }
        answer(id, dir ? dir : "", flag && flag[0] == '1', &o);
        req.n = 0;
    }
    gs_buf_free(&req);
    for (int i = 0; i < OPEN; i++) gs_repo_close(g_open[i]);
    return 0;
}
