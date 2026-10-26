#define _DEFAULT_SOURCE   /* DIR, readdir on the host */
/* Host-build stand-ins for the OS-backed pieces (builtins_os.c and
 * eval_extern.c are target-only: embk syscalls). The pure pipeline is fully
 * testable without them; OS command names simply don't resolve here and
 * fall through to the unknown-command error below. */
#include "builtins/builtins.h"
#include "eval/eval.h"
#include "prompt/wizard_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

builtin_fn builtin_lookup_os(const char *name) {
    (void)name;
    return NULL;   /* ls/cat/ps/... : not on the host */
}

const char *builtin_os_name(size_t i) {
    (void)i;
    return NULL;
}

int shell_list_dir(const char *path, void (*each)(void *ctx, const char *name, bool dir), void *ctx) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    for (struct dirent *de; (de = readdir(d)); n++) {
        char full[1024];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        each(ctx, de->d_name, stat(full, &st) == 0 && S_ISDIR(st.st_mode));
    }
    closedir(d);
    return n;
}

struct value extern_stage_run(const struct command *cmd, struct value input,
                              struct scope *env, bool to_terminal) {
    (void)env; (void)to_terminal;
    value_free(&input);
    char msg[96];
    snprintf(msg, sizeof msg, "unknown command '%s' (externals need the OS)", cmd->name);
    return value_error(msg);
}

/* `prompt configure` draws on a terminal; the host tests have none. */
int wizard_os_run(bool force, char *err, size_t cap) {
    (void)force;
    snprintf(err, cap, "prompt configure: it needs a terminal to draw its pages on");
    return -1;
}

/* Paths and whole files, the host's way: what `prompt import-p10k` reads in
 * the host tests. */
void shell_path_resolve(const char *in, char *out, size_t cap) {
    char cwd[512];
    if (in[0] == '/' || !getcwd(cwd, sizeof cwd)) snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "%s/%s", cwd, in);
}
struct value shell_read_file(const char *path, const char *who) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[400];
        snprintf(msg, sizeof msg, "%s: can't open '%s'", who, path);
        return value_error(msg);
    }
    char *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            char *grown = realloc(buf, cap = (cap ? cap * 2 : 8192));
            if (!grown) { free(buf); fclose(f); return value_error("out of memory"); }
            buf = grown;
        }
        size_t got = fread(buf + n, 1, cap - n - 1, f);
        if (!got) break;
        n += got;
    }
    fclose(f);
    struct value v = value_string_n(buf ? buf : "", n);
    free(buf);
    return v;
}

/* Files are the OS's: a redirected builtin says so on the host. */
struct value stage_redirect_input(const struct command *cmd, struct value input, struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    return value_error("redirections need the OS");
}
struct value stage_redirect_output(const struct command *cmd, struct value result, struct scope *env) {
    (void)cmd; (void)env;
    value_free(&result);
    return value_error("redirections need the OS");
}
