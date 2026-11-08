/* prompt_dump -- one prompt from shell/prompt's engine, for
 * tools/golden_prompt.py to hold against Powerlevel10k's own.
 *
 *   prompt_dump CONFIG... [--cols N] [--home DIR] [--pwd DIR] [--last-pwd DIR]
 *               [--user NAME] [--status N] [--duration SECONDS] [--jobs N]
 *               [--time H:M:S] [--git branch=main,staged=1,...]
 *               [--fsroot DIR]
 *
 * CONFIG is a .p10k.zsh, or several read in turn. The filesystem questions (unique prefixes, folder
 * markers) are answered under --fsroot, which stands for --home. Prints one
 * JSON object: left, right, transient, add_newline, lines, ignored. */
#include "prompt/p10k.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *g_home = "/tmp/emblink-prompt-home", *g_root;

/* A path under the home, where the fixture stands. */
static void real(const char *dir, char *out, size_t cap) {
    size_t hl = strlen(g_home);
    if (g_root && !strncmp(dir, g_home, hl) && (!dir[hl] || dir[hl] == '/')) snprintf(out, cap, "%s%s", g_root, dir + hl);
    else snprintf(out, cap, "%s", dir);
}

static bool list(void *ctx, const char *dir, void (*each)(void *arg, const char *name, bool dir), void *arg) {
    (void)ctx;
    char path[1024];
    real(dir, path, sizeof path);
    DIR *d = opendir(path);
    if (!d) return false;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1300];
        struct stat st;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        each(arg, e->d_name, !stat(sub, &st) && S_ISDIR(st.st_mode));
    }
    closedir(d);
    return true;
}
static bool writable(void *ctx, const char *dir) { char p[1024]; (void)ctx; real(dir, p, sizeof p); return !access(p, W_OK); }
static bool exists(void *ctx, const char *dir) { char p[1024]; struct stat st; (void)ctx; real(dir, p, sizeof p); return !stat(p, &st); }

static void json_str(const char *k, const char *s, bool comma) {
    printf("\"%s\": \"", k);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p == '\n') printf("\\n");
        else if (*p < 0x20 || *p == 0x7f) printf("\\u%04x", *p);
        else putchar(*p);
    }
    printf("\"%s", comma ? ", " : "");
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

static void git(struct p10k_git *g, char *spec) {
    g->repo = true;
    for (char *kv = strtok(spec, ","); kv; kv = strtok(NULL, ",")) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq++ = 0;
        int n = atoi(eq);
        if (!strcmp(kv, "branch")) snprintf(g->branch, sizeof g->branch, "%s", eq);
        else if (!strcmp(kv, "tag")) snprintf(g->tag, sizeof g->tag, "%s", eq);
        else if (!strcmp(kv, "commit")) snprintf(g->commit, sizeof g->commit, "%s", eq);
        else if (!strcmp(kv, "remote_branch")) snprintf(g->remote_branch, sizeof g->remote_branch, "%s", eq);
        else if (!strcmp(kv, "remote_url")) snprintf(g->remote_url, sizeof g->remote_url, "%s", eq);
        else if (!strcmp(kv, "action")) snprintf(g->action, sizeof g->action, "%s", eq);
        else if (!strcmp(kv, "summary")) snprintf(g->summary, sizeof g->summary, "%s", eq);
        else if (!strcmp(kv, "staged")) g->staged = n;
        else if (!strcmp(kv, "unstaged")) g->unstaged = n;
        else if (!strcmp(kv, "untracked")) g->untracked = n;
        else if (!strcmp(kv, "conflicted")) g->conflicted = n;
        else if (!strcmp(kv, "ahead")) g->ahead = n;
        else if (!strcmp(kv, "behind")) g->behind = n;
        else if (!strcmp(kv, "stashes")) g->stashes = n;
        else if (!strcmp(kv, "push_ahead")) g->push_ahead = n;
        else if (!strcmp(kv, "push_behind")) g->push_behind = n;
        else if (!strcmp(kv, "loading")) g->loading = n != 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: prompt_dump CONFIG... [options]\n"); return 2; }
    struct p10k *cfg = p10k_new();
    struct p10k_import rep;
    bool ok = true;
    unsigned set = 0;
    int first = 1;
    for (; first < argc && strncmp(argv[first], "--", 2); first++) {      /* each file in turn, as zsh sources them */
        char *text = slurp(argv[first]);
        if (!text) { perror(argv[first]); return 1; }
        ok = ok && p10k_import(cfg, text, &rep);
        set += rep.set;
        free(text);
    }
    rep.set = set;
    struct p10k_fs fs = { NULL, list, writable, exists };
    struct p10k_state st = { 0 };
    st.home = g_home;
    st.pwd = g_home;
    st.user = "yves";
    st.host = "emblink";
    st.status = -1;
    st.duration = -1;
    st.year = 2026; st.month = 10; st.mday = 26; st.wday = 1;
    st.os = "linux";
    st.fs = &fs;
    int cols = 100;
    for (int i = first; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--cols")) cols = atoi(v);
        else if (!strcmp(k, "--home")) st.home = g_home = v;
        else if (!strcmp(k, "--pwd")) st.pwd = v;
        else if (!strcmp(k, "--last-pwd")) st.last_prompt_pwd = v;
        else if (!strcmp(k, "--user")) st.user = v;
        else if (!strcmp(k, "--status")) st.status = atoi(v);
        else if (!strcmp(k, "--duration")) st.duration = atof(v);
        else if (!strcmp(k, "--jobs")) st.jobs = atoi(v);
        else if (!strcmp(k, "--time")) sscanf(v, "%d:%d:%d", &st.hour, &st.minute, &st.second);
        else if (!strcmp(k, "--git")) git(&st.git, argv[i + 1]);
        else if (!strcmp(k, "--fsroot")) g_root = v;
        else { fprintf(stderr, "prompt_dump: unknown option %s\n", k); return 2; }
    }
    st.first = st.status < 0;                         /* as the shell has it: no command yet */
    struct p10k_prompt p;
    if (!p10k_render(cfg, &st, cols, &p)) { fprintf(stderr, "prompt_dump: out of memory\n"); return 1; }
    printf("{");
    json_str("left", p.left, true);
    json_str("right", p.right, true);
    json_str("transient", p.transient, true);
    printf("\"add_newline\": %d, \"lines\": %d, \"imported\": %s, \"set\": %u, ", p.add_newline, p.lines,
           ok ? "true" : "false", rep.set);
    json_str("ignored", rep.ignored, true);
    json_str("error", rep.error, false);
    printf("}\n");
    p10k_prompt_free(&p);
    p10k_free(cfg);
    return 0;
}
