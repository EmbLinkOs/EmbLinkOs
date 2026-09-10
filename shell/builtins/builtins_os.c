/* ==========================================================================
 * builtins_os.c -- the OS-backed builtins: the standard command NAMES, each
 * a NATIVE structured implementation on EmbLink's own syscalls (nothing
 * shells out, nothing parses text):
 *
 *   ls cat cd pwd mkdir touch rm cp mv save ps kill uptime date clear
 *
 * TARGET-ONLY (embk syscalls); the host test build's builtin_lookup_os stub
 * returns NULL for all of these.
 *
 * The WORKING DIRECTORY is the LIBC's (chdir/getcwd), not this file's: `cd`
 * moves the real one and republishes PWD, which is how spawned commands learn
 * where they were run from. The kernel only speaks absolute paths, so every
 * path argument still goes through path_resolve() (join + "."/".."
 * normalization) before reaching a RAW SDK call -- those bypass the libc.
 * ========================================================================== */
#include "builtins/builtins.h"
#include "eval/exec.h"     /* the background-job table */
#include "sval/sval.h"
#include "hist/hist.h"
#include "embk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define LS_MAX_ENTRIES 128
#define PATH_MAX_LEN   192

/* NO g_cwd here any more. The working directory is the LIBC's (syscalls.c's
 * g_cwd, reached via chdir/getcwd) -- ONE source of truth. This file kept its
 * own copy back when chdir() was ENOSYS and the cwd was a shell-only fiction.
 *
 * The shell still resolves paths ITSELF (below) because its builtins call the
 * RAW SDK (embk_open/embk_stat/...), which bypasses the libc and its path_abs.
 * So: same cwd, resolved twice, deliberately -- not two different cwds. */

/* -------------------------------------------------------------------------
 * Path resolution: absolute passes through, relative joins g_cwd; then the
 * result is normalized segment-by-segment ("." dropped, ".." pops). The
 * output is always absolute, never ends in '/' except for the root itself.
 * ------------------------------------------------------------------------- */
static void path_resolve(const char *in, char *out, size_t cap) {
    char raw[PATH_MAX_LEN * 2];
    if (in && in[0] == '/') {
        snprintf(raw, sizeof raw, "%s", in);
    } else {
        /* Relative: against the REAL cwd, the one chdir() moves and every
         * spawned child inherits via PWD. */
        char cwd[PATH_MAX_LEN];
        if (!getcwd(cwd, sizeof cwd)) { cwd[0] = '/'; cwd[1] = '\0'; }
        snprintf(raw, sizeof raw, "%s/%s", cwd, in ? in : "");
    }

    /* normalize into out */
    size_t o = 0;
    out[o++] = '/';
    const char *p = raw;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        p += n;

        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 1 && out[o - 1] != '/') o--;   /* pop the last segment */
            if (o > 1) o--;                            /* and its slash */
            continue;
        }
        if (o > 1 && o < cap - 1) out[o++] = '/';
        else if (o == 1) { /* first segment sits right after the root '/' */ }
        for (size_t i = 0; i < n && o < cap - 1; i++) out[o++] = seg[i];
    }
    out[o] = '\0';
}

/* One resolved path argument (arg `idx`, or the default when absent). */
static const struct value *arg_err = NULL;   /* unused sentinel */
static int arg_path(const struct command *cmd, size_t idx, const char *dflt,
                    char *out, size_t cap) {
    (void)arg_err;
    const char *word = dflt;
    if (cmd->nargs > idx) {
        word = expr_as_word(cmd->args[idx]);
        if (!word) return -1;
    }
    if (!word) return -1;
    path_resolve(word, out, cap);
    return 0;
}

static struct value err_os(const char *what, const char *path, int rc) {
    char msg[128];
    snprintf(msg, sizeof msg, "%s: '%s' (err %d)", what, path, -rc);
    return value_error(msg);
}

/* -------------------------------------------------------------------------
 * ls [path] -> Table {name, type, size}
 * ------------------------------------------------------------------------- */
static const char *dt_name(uint8_t t) {
    switch (t) {
    case EMBK_DT_REG: return "file";
    case EMBK_DT_DIR: return "dir";
    case EMBK_DT_LNK: return "link";
    default:          return "?";
    }
}

static struct value bi_ls(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);

    char path[PATH_MAX_LEN];
    /* No argument -> "." -> path_resolve joins the real cwd. */
    if (arg_path(cmd, 0, ".", path, sizeof path) != 0)
        return value_error("ls: path must be a plain word or string");

    struct embk_dirent *ents =
        (struct embk_dirent *)malloc(LS_MAX_ENTRIES * sizeof(*ents));
    if (!ents) return value_error("out of memory");

    int64_t n = embk_readdir(path, ents, LS_MAX_ENTRIES);
    if (n < 0) { free(ents); return err_os("ls: can't read", path, (int)n); }

    struct value out = value_table();
    for (int64_t i = 0; i < n; i++) {
        struct value row = value_record();
        value_record_set(&row, "name", value_string(ents[i].name));
        value_record_set(&row, "type", value_string(dt_name(ents[i].type)));

        char full[PATH_MAX_LEN + 64];
        size_t plen = strlen(path);
        bool slash = plen > 0 && path[plen - 1] == '/';
        snprintf(full, sizeof full, "%s%s%s", path, slash ? "" : "/", ents[i].name);
        struct embk_stat st;
        bool have_st = (ents[i].type == EMBK_DT_REG || ents[i].type == EMBK_DT_DIR) &&
                       embk_stat(full, &st) == 0;

        if (have_st && ents[i].type == EMBK_DT_REG)
            value_record_set(&row, "size", value_filesize((int64_t)st.size));
        else
            value_record_set(&row, "size", value_null());

        /* modified: a real DATE value -- `where modified > ...` compares
         * numerically, the renderer prints it ISO-ish. 0 = fs untracked. */
        if (have_st && st.mtime > 0)
            value_record_set(&row, "modified", value_date((int64_t)st.mtime));
        else
            value_record_set(&row, "modified", value_null());

        value_table_push_row(&out, row);
    }
    free(ents);
    return out;
}

/* -------------------------------------------------------------------------
 * cd [dir] / pwd -- the shell-side working directory.
 * ------------------------------------------------------------------------- */
/* Keep PWD in step with the real cwd.
 *
 * This is not bookkeeping -- it is HOW a child learns where it was run from.
 * EmbLink inherits nothing: eval_extern hands `environ` to every external
 * command, crt0 seeds the child's cwd from PWD, and that chain is the whole
 * mechanism. Without this, `cd /proj` then `git status` would run git at "/".
 * Public so main.c can publish the startup directory too. */
void shell_cwd_publish(void) {
    char buf[PATH_MAX_LEN];
    if (getcwd(buf, sizeof buf)) setenv("PWD", buf, 1);
}

static struct value bi_cd(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    /* bare `cd` goes HOME and `cd -` returns whence you came -- the two
     * motions a hand types without thinking, straight from every Unix shell.
     * Both look at the RAW word: `-` resolved against the cwd would become
     * "/somewhere/-" and the intent would be gone before we saw it. */
    static char oldpwd[PATH_MAX_LEN];
    const char *home = getenv("HOME");
    const char *raw = cmd->nargs > 0 ? expr_as_word(cmd->args[0]) : NULL;
    if (cmd->nargs > 0 && !raw)
        return value_error("cd: path must be a plain word");
    if (raw && strcmp(raw, "-") == 0) {
        if (!oldpwd[0]) return value_error("cd: no previous directory");
        snprintf(path, sizeof path, "%s", oldpwd);
    } else if (!raw) {
        snprintf(path, sizeof path, "%s", home && *home ? home : "/");
    } else {
        path_resolve(raw, path, sizeof path);
    }
    char before[PATH_MAX_LEN];
    if (!getcwd(before, sizeof before)) before[0] = 0;

    /* chdir() does the verifying (exists + is a directory) -- no need to stat
     * first, and no window where we could disagree with it. */
    if (chdir(path) != 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "cd: %s: %s", path,
                 errno == ENOTDIR ? "not a directory" : "no such directory");
        return value_error(msg);
    }
    if (before[0]) snprintf(oldpwd, sizeof oldpwd, "%s", before);
    shell_cwd_publish();
    return value_null();
}

static struct value bi_pwd(const struct command *cmd, struct value input,
                           struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    char buf[PATH_MAX_LEN];
    if (!getcwd(buf, sizeof buf)) return value_error("pwd: cannot read the cwd");
    return value_path(buf);
}

/* -------------------------------------------------------------------------
 * cat <file> -> the contents, as a String value (pipe it to wc, =~, save).
 * ------------------------------------------------------------------------- */
static struct value bi_cat(const struct command *cmd, struct value input,
                           struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("cat: takes a file path");

    int fd = (int)embk_open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return err_os("cat: can't open", path, fd);

    size_t cap = 4096, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { embk_close(fd); return value_error("out of memory"); }
    for (;;) {
        if (n + 1024 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); embk_close(fd); return value_error("out of memory"); }
            buf = nb;
        }
        int64_t got = embk_read(fd, buf + n, 1024);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    struct value out = value_string_n(buf, n);
    free(buf);
    return out;
}

/* -------------------------------------------------------------------------
 * mkdir / touch / rm
 * ------------------------------------------------------------------------- */
static struct value bi_mkdir(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("mkdir: takes a directory path");
    int rc = embk_mkdir(path);
    if (rc != 0) return err_os("mkdir: can't create", path, rc);
    return value_null();
}

static struct value bi_touch(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("touch: takes a file path");
    int fd = (int)embk_open(path, EMBK_O_CREAT | EMBK_O_WRONLY, 0644);
    if (fd < 0) return err_os("touch: can't create", path, fd);
    embk_close(fd);
    return value_null();
}

static struct value bi_rm(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("rm: takes a file path");
    int rc = embk_unlink(path);
    if (rc != 0) return err_os("rm: can't remove", path, rc);
    return value_null();
}

static struct value bi_rmdir(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env;
    value_free(&input);
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("rmdir: takes a directory path");
    int rc = embk_rmdir(path);
    if (rc != 0) return err_os("rmdir: can't remove (not empty?)", path, rc);
    return value_null();
}

/* -------------------------------------------------------------------------
 * cp <src> <dst> / mv <src> <dst>. Byte copy through a bounce buffer.
 * (No O_TRUNC in the kernel yet: overwriting a LONGER existing file leaves
 * its tail -- copy to fresh names; documented limitation.)
 * ------------------------------------------------------------------------- */
static struct value copy_file(const char *verb, const char *src, const char *dst) {
    int in = (int)embk_open(src, 0, 0);
    if (in < 0) return err_os(verb, src, in);
    int out = (int)embk_open(dst, EMBK_O_CREAT | EMBK_O_WRONLY | EMBK_O_TRUNC, 0644);
    if (out < 0) { embk_close(in); return err_os(verb, dst, out); }

    char buf[1024];
    for (;;) {
        int64_t got = embk_read(in, buf, sizeof buf);
        if (got < 0) { embk_close(in); embk_close(out); return err_os(verb, src, (int)got); }
        if (got == 0) break;
        int64_t off = 0;
        while (off < got) {
            int64_t w = embk_write(out, buf + off, (size_t)(got - off));
            if (w <= 0) { embk_close(in); embk_close(out); return err_os(verb, dst, (int)w); }
            off += w;
        }
    }
    embk_close(in);
    embk_close(out);
    return value_null();
}

static struct value bi_cp(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char src[PATH_MAX_LEN], dst[PATH_MAX_LEN];
    if (cmd->nargs < 2 ||
        arg_path(cmd, 0, NULL, src, sizeof src) != 0 ||
        arg_path(cmd, 1, NULL, dst, sizeof dst) != 0)
        return value_error("cp: takes <src> <dst>");
    return copy_file("cp", src, dst);
}

static struct value bi_mv(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);
    char src[PATH_MAX_LEN], dst[PATH_MAX_LEN];
    if (cmd->nargs < 2 ||
        arg_path(cmd, 0, NULL, src, sizeof src) != 0 ||
        arg_path(cmd, 1, NULL, dst, sizeof dst) != 0)
        return value_error("mv: takes <src> <dst>");
    struct value rc = copy_file("mv", src, dst);
    if (rc.type == VAL_ERROR) return rc;
    int u = embk_unlink(src);
    if (u != 0) return err_os("mv: copied, but can't remove", src, u);
    return value_null();
}

/* -------------------------------------------------------------------------
 * save <path> -- write the PIPED value to a file, rendered as text
 * (`ls | save /listing.txt`, `cat a.txt | save /b.txt`).
 * ------------------------------------------------------------------------- */
static struct value bi_save(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)env;
    char path[PATH_MAX_LEN];
    if (cmd->nargs < 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0) {
        value_free(&input);
        return value_error("save: takes a destination path");
    }
    if (input.type == VAL_NULL) return value_error("save: nothing to save (pipe a value in)");

    int fd = (int)embk_open(path, EMBK_O_CREAT | EMBK_O_WRONLY | EMBK_O_TRUNC, 0644);
    if (fd < 0) { value_free(&input); return err_os("save: can't create", path, fd); }
    int rc = sval_print(&input, fd);
    embk_close(fd);
    value_free(&input);
    if (rc != 0) return err_os("save: write failed", path, rc);
    return value_null();
}

/* -------------------------------------------------------------------------
 * ps -> Table {pid, ppid, state, pri, kind}   |   kill <pid>
 * ------------------------------------------------------------------------- */
static struct value bi_ps(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    static const char *state_names[] = { "unused", "ready", "running", "blocked", "zombie" };

    struct embk_proc_info info[64];
    int n = embk_proc_list(info, 64);
    if (n < 0) return value_error("ps: proc_list failed");

    struct value out = value_table();
    for (int i = 0; i < n; i++) {
        struct value r = value_record();
        value_record_set(&r, "pid",   value_int((int64_t)info[i].pid));
        value_record_set(&r, "ppid",  value_int((int64_t)info[i].parent_pid));
        value_record_set(&r, "state", value_string(
            (info[i].state >= 0 && info[i].state <= 4) ? state_names[info[i].state] : "?"));
        value_record_set(&r, "pri",   value_int((int64_t)info[i].priority));
        value_record_set(&r, "kind",  value_string(info[i].is_kthread ? "kthread" : "process"));
        /* CPU actually consumed, in MILLISECONDS. A column rather than a
         * pretty string, so it sorts and filters like everything else here:
         *   ps | sort-by cpu | last 5      -- what is this machine busy with
         *   ps | where cpu > 1000          -- who has burned a second
         * Milliseconds because nanoseconds is precision nobody reading a
         * process list is asking for, and it would push the column wide. */
        value_record_set(&r, "cpu",   value_int((int64_t)(info[i].cpu_ns / 1000000ULL)));
        value_table_push_row(&out, r);
    }
    return out;
}

/* -------------------------------------------------------------------------
 * env -- the environment this shell will hand to the children it spawns.
 *
 * EmbLink inherits NOTHING: a child gets an environment only because a parent
 * passes one (kernel spawn.h). eval_extern.c passes THIS -- so what `env` shows
 * is exactly what the next external command will receive, not an approximation.
 *
 *   env                 -> table {name, value}
 *   env set KEY VALUE   -> set/overwrite
 *   env unset KEY       -> remove
 * ------------------------------------------------------------------------- */
static struct value bi_env(const struct command *cmd, struct value input,
                           struct scope *env) {
    value_free(&input);

    if (cmd->nargs == 0) {
        struct value out = value_table();
        for (int i = 0; environ && environ[i]; i++) {
            const char *e = environ[i];
            const char *eq = strchr(e, '=');
            /* No '=' should be impossible, but show it rather than hide it:
             * silently dropping a row would make a broken entry invisible. */
            struct value r = value_record();
            if (!eq) {
                value_record_set(&r, "name",  value_string(e));
                value_record_set(&r, "value", value_null());
            } else {
                char name[128];
                size_t nlen = (size_t)(eq - e);
                if (nlen >= sizeof name) nlen = sizeof name - 1;
                memcpy(name, e, nlen);
                name[nlen] = '\0';
                value_record_set(&r, "name",  value_string(name));
                value_record_set(&r, "value", value_string(eq + 1));
            }
            value_table_push_row(&out, r);
        }
        return out;
    }

    /* Sub-commands take BARE WORDS, not expressions: `env set HOME /` should not
     * try to evaluate HOME as a variable. cmd->args carry the raw text. */
    const char *sub = cmd->args[0] ? expr_as_word(cmd->args[0]) : NULL;
    if (!sub) return value_error("env: usage: env | env set KEY VALUE | env unset KEY");

    if (strcmp(sub, "set") == 0) {
        if (cmd->nargs != 3) return value_error("env set: needs KEY and VALUE");
        const char *k = expr_as_word(cmd->args[1]);
        const char *v = expr_as_word(cmd->args[2]);
        if (!k || !v) return value_error("env set: KEY and VALUE must be words");
        if (strchr(k, '=')) return value_error("env set: KEY cannot contain '='");
        if (setenv(k, v, 1) != 0) return value_error("env set: out of memory");
        return value_null();
    }
    if (strcmp(sub, "unset") == 0) {
        if (cmd->nargs != 2) return value_error("env unset: needs a KEY");
        const char *k = expr_as_word(cmd->args[1]);
        if (!k) return value_error("env unset: KEY must be a word");
        /* unsetenv of an absent name succeeds -- removing what isn't there is
         * not an error, it is the requested end state. */
        if (unsetenv(k) != 0) return value_error("env unset: invalid KEY");
        return value_null();
    }
    return value_error("env: usage: env | env set KEY VALUE | env unset KEY");
}

static struct value bi_kill(const struct command *cmd, struct value input,
                            struct scope *env) {
    value_free(&input);
    if (cmd->nargs != 1) return value_error("kill: takes a pid (see ps)");
    struct value pv = expr_eval(cmd->args[0], env);
    if (pv.type == VAL_ERROR) return pv;
    if (pv.type != VAL_INT || pv.u.i <= 0) {
        value_free(&pv);
        return value_error("kill: pid must be a positive integer");
    }
    uint32_t pid = (uint32_t)pv.u.i;
    int rc = embk_proc_kill(pid);
    if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "kill: no live process with pid %u", pid);
        return value_error(msg);
    }
    return value_null();
}

/* -------------------------------------------------------------------------
 * uptime / date / clear
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * The identity-and-orientation set every Unix hand types on reflex.
 * ------------------------------------------------------------------------- */
static struct value bi_whoami(const struct command *cmd, struct value input,
                              struct scope *env) {
    (void)cmd; (void)env; value_free(&input);
    const char *u = getenv("USER");
    return value_string(u && *u ? u : "user");
}

static struct value bi_hostname(const struct command *cmd, struct value input,
                                struct scope *env) {
    (void)cmd; (void)env; value_free(&input);
    const char *h = getenv("HOSTNAME");
    return value_string(h && *h ? h : "emblink");
}

/* history -> Table {n, command}: the shell's own ring, oldest first, numbered
 * the way `history` numbers -- so the output pipes ("history | last 5"). */
static struct value bi_history(const struct command *cmd, struct value input,
                               struct scope *env) {
    (void)cmd; (void)env; value_free(&input);
    struct value out = value_table();
    size_t n = hist_count();
    for (size_t i = 0; i < n; i++) {
        struct value row = value_record();
        value_record_set(&row, "n", value_int((int64_t)(i + 1)));
        value_record_set(&row, "command", value_string(hist_get(i)));
        value_table_push_row(&out, row);
    }
    return out;
}

/* which <name>: answer the question the way the shell itself would resolve it
 * -- builtins first (both dispatch tables), then the extern search order
 * eval_extern actually uses. Guessing differently would be lying. */
static struct value bi_which(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)env; value_free(&input);
    const char *name = cmd->nargs > 0 ? expr_as_word(cmd->args[0]) : NULL;
    if (!name) return value_error("which: which <command>");
    if (builtin_lookup(name) || builtin_lookup_os(name))
        return value_string("shell builtin");
    char path[256];
    struct embk_stat st;
    snprintf(path, sizeof path, "/system/bin/%s.elf", name);
    if (embk_stat(path, &st) == 0) return value_string(path);
    snprintf(path, sizeof path, "/data/apps/%s/%s.elf", name, name);
    if (embk_stat(path, &st) == 0) return value_string(path);
    char msg[128];
    snprintf(msg, sizeof msg, "which: %s: not found", name);
    return value_error(msg);
}

/* clip: the piped value's text goes to the SYSTEM clipboard, so the shell's
 * structured world can feed every paste target in the GUI ("cat notes | clip",
 * then Ctrl+V anywhere). Copy-without-selection: the pipeline IS the
 * selection. Scalars and strings in v1; tables have `save`. */
static struct value bi_clip(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)cmd; (void)env;
    char small[256];
    const char *text = NULL; size_t len = 0;
    if (input.type == VAL_STRING || input.type == VAL_PATH) {
        text = input.u.s.bytes; len = input.u.s.len;
    } else if (input.type == VAL_TABLE || input.type == VAL_LIST ||
               input.type == VAL_RECORD) {
        value_free(&input);
        return value_error("clip: pipe text (tables: use `save`)");
    } else {
        len = sval_format_scalar(&input, small, sizeof small);
        text = small;
    }
    int rc = embk_clip_set(text, len);
    value_free(&input);
    if (rc != 0) return value_error("clip: could not set the clipboard");
    return value_null();
}

/* ==========================================================================
 * glob PATTERN -- file selection by wildcard.
 *
 * `ls *.txt` is what a person types. This shell's structured answer --
 * `ls | where name =~ ".txt"` -- works and composes and is not what anyone
 * reaches for first, so the pattern gets a name of its own:
 *
 *     glob "*.c"                  -> a table, the same shape ls produces
 *     for f in $(glob "src/*.c") { ... }
 *     glob "*.tmp" | count
 *
 * A TABLE with ls's columns rather than a bare list of names, deliberately:
 * it means every transform already written works on the result
 * (`glob "*.log" | where size > 1mb | sort-by modified`), where a list of
 * strings would have dead-ended.
 *
 * WHAT IS AND IS NOT MATCHED. `*` matches any run of characters WITHIN one
 * path component and `?` matches exactly one. `*` deliberately does not cross
 * '/': `*.c` must not reach into subdirectories, because a pattern that
 * silently recursed would make `rm *.tmp` a very different command from the
 * one it looks like. Recursive matching wants its own spelling (`**`) and is
 * not here yet.
 *
 * A pattern that matches NOTHING yields an empty table -- not an error, and
 * not the pattern itself passed through as a literal filename, which is the
 * single worst thing the Bourne shell does. `for f in $(glob "*.none") { }`
 * running zero times is what every caller means.
 * ========================================================================== */

/* One path component against one pattern component. Iterative with a
 * backtrack point rather than recursive: a pattern is user input, and a
 * recursive matcher on `a*a*a*a*b` is a stack the caller chooses the depth
 * of. */
static bool glob_match_1(const char *pat, size_t plen, const char *str, size_t slen) {
    size_t p = 0, s = 0, star = (size_t)-1, mark = 0;

    while (s < slen) {
        if (p < plen && (pat[p] == '?' || pat[p] == str[s])) { p++; s++; continue; }
        if (p < plen && pat[p] == '*') { star = p++; mark = s; continue; }
        if (star != (size_t)-1) { p = star + 1; s = ++mark; continue; }
        return false;
    }
    while (p < plen && pat[p] == '*') p++;
    return p == plen;
}

static bool has_glob_chars(const char *s) {
    for (; *s; s++)
        if (*s == '*' || *s == '?') return true;
    return false;
}

/* ln TARGET LINK -- make a symbolic link.
 *
 * NO `-s` FLAG, for two reasons that agree.
 *
 * There is nothing for it to distinguish: this OS has symbolic links and does
 * not have hard links, so `ln` means one thing. A flag whose only job is to
 * select the only option is ceremony.
 *
 * And it could not be written anyway. This shell's word rules make a LEADING
 * '-' an operator -- `-` joins a word only when flanked by word characters on
 * both sides -- so `-s` lexes as a minus applied to `s`, not as a flag. That is
 * the documented grammar, and bending it here to accept one command's dash
 * would be a special case in the LEXER, paid for by every line anyone ever
 * writes. The house convention is subcommand words (`env set`, `pkg install`),
 * and this fits it.
 *
 * The target is not resolved -- a link to something that does not exist yet is
 * the normal case, not a mistake to catch. */
static struct value bi_ln(const struct command *cmd, struct value input,
                          struct scope *env) {
    (void)env;
    value_free(&input);

    if (cmd->nargs != 2)
        return value_error("ln: usage: ln <target> <linkname>  "
                           "(no -s: symbolic is the only kind here)");

    const char *target = expr_as_word(cmd->args[0]);
    const char *linkw  = expr_as_word(cmd->args[1]);
    if (!target || !linkw)
        return value_error("ln: target and link must be plain words or strings");

    /* The LINK's path is resolved against the cwd, because that is where the
     * name is being created. The TARGET is passed through untouched -- turning
     * a relative target absolute here would silently change what the link
     * means if the tree is ever moved. */
    char linkpath[PATH_MAX_LEN];
    path_resolve(linkw, linkpath, sizeof linkpath);

    int rc = embk_symlink(target, linkpath);
    if (rc < 0) return err_os("ln: can't create", linkpath, rc);

    struct value out = value_record();
    value_record_set(&out, "link",   value_path(linkpath));
    value_record_set(&out, "target", value_string(target));
    return out;
}

/* readlink PATH -- the text a link holds, without following it. */
static struct value bi_readlink(const struct command *cmd, struct value input,
                                struct scope *env) {
    (void)env;
    value_free(&input);

    char path[PATH_MAX_LEN];
    if (cmd->nargs != 1 || arg_path(cmd, 0, NULL, path, sizeof path) != 0)
        return value_error("readlink: usage: readlink <path>");

    char buf[PATH_MAX_LEN];
    int64_t n = embk_readlink(path, buf, sizeof buf - 1);
    if (n < 0) return err_os("readlink", path, (int)n);
    if ((size_t)n >= sizeof buf) return value_error("readlink: target is too long");
    buf[n] = '\0';
    return value_string(buf);
}

static struct value err_glob(const char *what) {
    char msg[128];
    snprintf(msg, sizeof msg, "glob: %s", what);
    return value_error(msg);
}

static struct value bi_glob(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)env;
    value_free(&input);

    if (cmd->nargs != 1)
        return err_glob("takes one pattern");

    const char *pat = expr_as_word(cmd->args[0]);
    if (!pat)
        return err_glob("pattern must be a plain word or string");

    /* Split the pattern into "the directory to read" and "the name pattern".
     * Only the LAST component may contain wildcards -- a wildcard in a
     * directory component would need to walk a tree, which is the recursive
     * case this deliberately does not do yet. */
    const char *slash = strrchr(pat, '/');
    char dir[PATH_MAX_LEN];
    const char *namepat;

    if (slash) {
        size_t dlen = (size_t)(slash - pat);
        if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }       /* "/foo" -> "/" */
        else {
            if (dlen >= sizeof dir) return err_glob("pattern is too long");
            memcpy(dir, pat, dlen);
            dir[dlen] = '\0';
        }
        namepat = slash + 1;
        if (has_glob_chars(dir))
            return err_glob("wildcards are only allowed in the last path component");
    } else {
        dir[0] = '.'; dir[1] = '\0';
        namepat = pat;
    }

    char resolved[PATH_MAX_LEN];
    path_resolve(dir, resolved, sizeof resolved);

    struct embk_dirent *ents =
        (struct embk_dirent *)malloc(LS_MAX_ENTRIES * sizeof(*ents));
    if (!ents) return value_error("out of memory");

    int64_t n = embk_readdir(resolved, ents, LS_MAX_ENTRIES);
    if (n < 0) { free(ents); return err_os("glob: can't read", resolved, (int)n); }

    size_t patlen = strlen(namepat);
    struct value out = value_table();

    for (int64_t i = 0; i < n; i++) {
        if (!glob_match_1(namepat, patlen, ents[i].name, strlen(ents[i].name)))
            continue;

        /* "." and ".." are never matched by a wildcard, only by naming them.
         * `rm *` reaching ".." is not a hypothetical class of accident. */
        if (has_glob_chars(namepat) &&
            (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0))
            continue;

        struct value row = value_record();
        value_record_set(&row, "name", value_string(ents[i].name));
        value_record_set(&row, "type", value_string(dt_name(ents[i].type)));

        /* The full path as a PATH value, because that is what the caller
         * feeds back to `rm`/`cat`. Without it every loop over a glob has to
         * re-join the directory by hand and half of them get it wrong. */
        char full[PATH_MAX_LEN + 64];
        size_t rlen = strlen(resolved);
        bool slash_end = rlen > 0 && resolved[rlen - 1] == '/';
        snprintf(full, sizeof full, "%s%s%s", resolved, slash_end ? "" : "/", ents[i].name);
        value_record_set(&row, "path", value_path(full));

        struct embk_stat st;
        bool have_st = (ents[i].type == EMBK_DT_REG || ents[i].type == EMBK_DT_DIR) &&
                       embk_stat(full, &st) == 0;
        value_record_set(&row, "size",
                         (have_st && ents[i].type == EMBK_DT_REG)
                             ? value_filesize((int64_t)st.size) : value_null());
        value_record_set(&row, "modified",
                         (have_st && st.mtime > 0) ? value_date((int64_t)st.mtime) : value_null());

        value_table_push_row(&out, row);
    }
    free(ents);
    return out;
}

/* --- background jobs -------------------------------------------------------
 *
 * `jobs` is a TABLE like everything else here, so it composes:
 *   jobs | where state == "running" | count
 * A structured shell that printed a special-cased job listing would be
 * throwing away the one property that makes it worth using.
 *
 * A finished job is shown ONCE and then forgotten. Keeping it forever fills
 * the table; dropping it the moment it exits would let a job finish between
 * two prompts and leave no trace that it ever ran -- which is exactly the
 * case a person needs to see. */
static struct value bi_jobs(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);

    jobs_poll();

    struct value out = value_table();
    /* Walk by index and re-fetch, because reporting a finished job FREES its
     * slot and shifts everything after it. */
    for (size_t i = 0; i < JOB_MAX; ) {
        struct job *j = jobs_at(i);
        if (!j) break;

        struct value row = value_record();
        value_record_set(&row, "job",   value_int(j->id));
        value_record_set(&row, "state", value_string(j->reaped ? "done" : "running"));
        value_record_set(&row, "exit",  j->reaped ? value_int(j->status) : value_null());
        value_record_set(&row, "cmd",   value_string(j->cmd ? j->cmd : ""));
        value_table_push_row(&out, row);

        if (j->reaped) jobs_forget(j);   /* shown once; the slot is free now */
        else i++;
    }
    return out;
}

/* `fg N` -- wait for job N and hand back its exit status.
 *
 * Not "bring it to the foreground" in the terminal sense: there is no terminal
 * ownership to transfer, because a background job here is a separate shell
 * with its own stdio. What a person actually wants from `fg` is "block until
 * that finishes", and that is what this does -- honestly named for what it
 * gives rather than borrowed from a mechanism this OS does not have. */
static struct value bi_fg(const struct command *cmd, struct value input,
                          struct scope *env) {
    value_free(&input);
    if (cmd->nargs != 1)
        return value_error("fg takes one job id");

    struct value idv = expr_eval(cmd->args[0], env);
    if (idv.type == VAL_ERROR) return idv;
    if (idv.type != VAL_INT) { value_free(&idv); return value_error("fg takes a job id"); }
    int id = (int)idv.u.i;
    value_free(&idv);

    struct job *j = jobs_by_id(id);
    if (!j) return value_error("no such job");

    int status = jobs_wait(j);
    struct value out = value_record();
    value_record_set(&out, "job",  value_int(id));
    value_record_set(&out, "exit", value_int(status));
    value_record_set(&out, "cmd",  value_string(j->cmd ? j->cmd : ""));
    jobs_forget(j);
    return out;
}

static struct value bi_uptime(const struct command *cmd, struct value input,
                              struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    uint64_t ms = embk_uptime_ms();
    uint64_t s = ms / 1000;
    char pretty[32];
    snprintf(pretty, sizeof pretty, "%lu:%02lu:%02lu",
             (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
             (unsigned long)(s % 60));
    struct value out = value_record();
    value_record_set(&out, "seconds", value_int((int64_t)s));
    value_record_set(&out, "pretty",  value_string(pretty));
    return out;
}

static struct value bi_date(const struct command *cmd, struct value input,
                            struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    time_t now = time(NULL);
    if (now <= 0) return value_error("date: no wall clock");
    return value_date((int64_t)now);   /* a DATE value -- renders ISO-ish,
                                        * compares numerically in `where` */
}

static struct value bi_clear(const struct command *cmd, struct value input,
                             struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    write(1, "\f", 1);   /* the terminal clears on form-feed; harmless on serial */
    return value_null();
}

/* -------------------------------------------------------------------------
 * The OS registry (builtin_lookup falls through to this).
 * ------------------------------------------------------------------------- */
builtin_fn builtin_lookup_os(const char *name) {
    static const struct { const char *name; builtin_fn fn; } tab[] = {
        { "ls",     bi_ls     }, { "cat",    bi_cat    },
        { "cd",     bi_cd     }, { "pwd",    bi_pwd    },
        { "mkdir",  bi_mkdir  }, { "rmdir",  bi_rmdir  },
        { "touch",  bi_touch  },
        { "rm",     bi_rm     }, { "cp",     bi_cp     },
        { "mv",     bi_mv     }, { "save",   bi_save   },
        { "ps",     bi_ps     }, { "kill",   bi_kill   },
        { "env",    bi_env    },
        { "uptime", bi_uptime }, { "date",   bi_date   },
        { "whoami", bi_whoami }, { "hostname", bi_hostname },
        { "history", bi_history }, { "which", bi_which },
        { "clip",    bi_clip    },
        { "clear",  bi_clear  },
        { "jobs",   bi_jobs   }, { "fg",     bi_fg     },
        { "glob",   bi_glob   },
        { "ln",     bi_ln     }, { "readlink", bi_readlink },
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].fn;
    return NULL;
}
