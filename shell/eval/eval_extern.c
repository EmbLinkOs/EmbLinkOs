/* ==========================================================================
 * eval_extern.c -- one EXTERNAL pipeline stage, and redirections. TARGET-ONLY
 * (embk syscalls); the host test build links shell/test/host_stubs.c instead.
 *
 * TWO KINDS OF PROGRAM. A program that links the shell SDK speaks the wire
 * and carries the SDK's mark (sval.c): it gets
 *   - pipe A's write end as its fd 3, where its structured output goes, and
 *   - pipe B's read end as its fd 0 when the stage has input, carrying the
 *     previous stage's value as wire frames.
 * Every other program -- sh, a port, anything that prints -- is a TEXT
 * program, and gets what a text program understands:
 *   - the input rendered as text (what a person would see) on its stdin, and
 *   - its stdout captured as one string when anything consumes it: a later
 *     stage, or `$(...)`. At the END of a statement nothing does, and it keeps
 *     the terminal -- which is what makes `sh`, an editor, anything that
 *     talks to a person, work from this prompt.
 * Redirections (`out>`, `err>`, `in<`, ...) are spawn actions on fds 0-2. A
 * structured program whose output is redirected is given no fd 3, so what it
 * prints for a person is what lands in the file.
 *
 * COPY semantics everywhere: after the spawn, the shell MUST close its own
 * handles to the ends it gave the child, or the child never sees EOF on stdin
 * and the shell never sees EOF on the output pipe.
 *
 * STREAMING I/O: the shell never writes ALL input before reading any output.
 * The pump interleaves -- feed stdin whenever the pipe has space (fd_avail on a
 * WRITE end = free bytes), drain the output whenever it has data -- so a child
 * that emits while its stdin is still arriving, with more than the pipe
 * buffer on BOTH sides, can't deadlock the shell. Once stdin is fully fed and
 * closed, the remainder is a plain drain.
 * ========================================================================== */
#include "eval/eval.h"
#include "builtins/builtins.h"
#include "sval/sval.h"
#include "wire/wire.h"
#include "lex/lex.h"
#include "embk.h"
#include "exec.h"       /* jobs_adopt_stopped -- ^Z turns a command into a job */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

extern void shell_tty_suspend(void);   /* main.c: cooked for the child's lifetime */
extern void shell_tty_resume(void);
extern void shell_interrupt_route(int handle);
extern char **environ;

/* Scratch fds for the shell's own ends of the plumbing. Above stdio+fd3,
 * below anything vfs_open would hand out mid-pipeline in practice. */
#define EXT_FD_INPUT_WRITE 10
#define EXT_FD_OUTPUT_READ 11

#define EXT_EPIPE 32   /* -EMBK_EPIPE (errno.h is kernel-only) -- the ONE write
                        * error that means "child hung up stdin", vs a transient
                        * fault we must retry instead of truncating the frame. */
#define EXT_ENOENT 2
#define EXT_ENOEXEC 8

/* THE LIMITS ON WHAT A PROGRAM IS GIVEN are the kernel's (kernel/process/
 * spawn.h, process.c): at most 127 arguments, and the argument strings, the
 * environment's and both pointer arrays together in the ONE 4 KiB page a new
 * program's stack starts with. They are checked here, before the spawn, so
 * the error can say which limit and by how much -- not an E2BIG, and never a
 * quietly shortened argument (which is what an 8-argument array of 96-byte
 * slots did here before). */
#define ARGV_MAX       127
#define ARG_PAGE       4096u
#define ROOM(bytes, n) ((bytes) + ((size_t)(n) + 1) * 8u + 16u)

/* What a text program prints into a pipeline is kept whole, up to the size
 * a structured frame may be. */
#define TEXT_CAPTURE_MAX (16u << 20)

/* Fold one collected frame into the stage result: 0 frames -> that value,
 * 2+ -> a list. Shared by the pump and the tail drain. */
static void absorb_frame(struct value *collected, size_t *nframes, struct value v) {
    if (*nframes == 0) {
        *collected = v;
    } else if (*nframes == 1) {
        struct value list = value_list();
        value_list_push(&list, *collected);
        value_list_push(&list, v);
        *collected = list;
    } else {
        value_list_push(collected, v);
    }
    (*nframes)++;
}

/* Grace-then-kill: the ESCALATION half of docs/INTERRUPTION.md §4.3, and it is
 * deliberately SHELL policy, not kernel law.
 *
 *   ^C -> kernel cancels the child (polite; it may clean up and exit)
 *         child still alive GRACE_MS later -> embk_kill (uncatchable)
 *
 * The shell never sees the ^C -- the kernel consumed it -- so the clock starts
 * from OBSERVING the child's cancel state (embk_child_cancelled), which is what
 * keeps "cancelled but declining" distinguishable from "healthy but slow": a
 * slow child that was never cancelled is never killed by this.
 *
 * 1500ms: long enough for real cleanup (delete a lock file, flush a buffer),
 * short enough that a ^C visibly works. Tunable here, visibly, like any policy. */
#define SHELL_CANCEL_GRACE_MS 1500

struct cancel_watch { uint64_t seen_ms; bool killed; };

static void cancel_watch_tick(struct cancel_watch *w, int64_t child) {
    if (w->killed) return;
    if (embk_child_cancelled((int)child) != 1) return;
    uint64_t now = embk_uptime_ms();
    if (w->seen_ms == 0) { w->seen_ms = now; return; }   /* clock starts */
    if (now - w->seen_ms >= SHELL_CANCEL_GRACE_MS) {
        (void)embk_kill((int)child);   /* exit closes its fds -> our pipe EOFs */
        w->killed = true;
    }
}

/* ==========================================================================
 * Which program, and what kind
 * ========================================================================== */
static bool is_file(const char *path) {
    struct embk_stat st;
    return embk_stat(path, &st) == 0 && st.type != EMBK_DT_DIR;
}

/* Resolution (docs/USERSPACE.md D3 §4.2): a name with a '/' is a path, made
 * absolute against the cwd -- `./build.esh`, `../bin/tool`. A bare name is
 * searched against an ORDERED, shell-owned list -- /system/bin first, then
 * /data/apps/<name>/ -- taking the FIRST that exists. There is deliberately NO
 * PATH env var (§4.2: that is ambient authority the per-process namespace
 * already grants); the order is policy and lives HERE, in one visible place.
 * /system/bin/NAME without .elf is a script installed under its own name. */
int shell_resolve_program(const char *name, char *path, size_t cap) {
    if (strchr(name, '/')) {
        shell_path_resolve(name, path, cap);
        return is_file(path) ? 0 : -1;
    }
    snprintf(path, cap, "/system/bin/%s.elf", name);
    if (is_file(path)) return 0;
    snprintf(path, cap, "/system/bin/%s", name);
    if (is_file(path)) return 0;
    snprintf(path, cap, "/data/apps/%s/%s.elf", name, name);
    if (is_file(path)) return 0;
    return -1;
}

static uint64_t le(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* Does this ELF64 file have a section called `want`? Read straight from the
 * section table: the header names it, the section-name string table names
 * every section. Anything malformed is simply "no". */
static bool elf_has_section(int fd, const char *want) {
    uint8_t eh[64];
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, eh, sizeof eh) != (ssize_t)sizeof eh) return false;
    if (memcmp(eh, "\x7f" "ELF", 4) || eh[4] != 2) return false;          /* 64-bit only */
    uint64_t shoff = le(eh + 0x28, 8);
    unsigned shentsize = (unsigned)le(eh + 0x3a, 2), shnum = (unsigned)le(eh + 0x3c, 2), shstrndx = (unsigned)le(eh + 0x3e, 2);
    if (!shoff || shentsize < 64 || !shnum || shnum > 4096 || shstrndx >= shnum) return false;
    uint8_t sh[64];
    if (lseek(fd, (off_t)(shoff + (uint64_t)shstrndx * shentsize), SEEK_SET) < 0 || read(fd, sh, 64) != 64) return false;
    uint64_t stroff = le(sh + 0x18, 8), strsize = le(sh + 0x20, 8);
    if (!strsize || strsize > 65536) return false;
    char *names = (char *)malloc((size_t)strsize + 1);
    if (!names) return false;
    bool found = false;
    if (lseek(fd, (off_t)stroff, SEEK_SET) >= 0 && read(fd, names, (size_t)strsize) == (ssize_t)strsize) {
        names[strsize] = 0;
        for (unsigned i = 0; i < shnum && !found; i++) {
            if (lseek(fd, (off_t)(shoff + (uint64_t)i * shentsize), SEEK_SET) < 0 || read(fd, sh, 64) != 64) break;
            uint64_t at = le(sh, 4);
            if (at < strsize && !strcmp(names + at, want)) found = true;
        }
    }
    free(names);
    return found;
}

/* A program's kind, remembered: stages run in loops, and a program file does
 * not change kind while the shell runs. Keyed by path. */
#define KIND_CACHE 32
static struct { char path[192]; int kind; } g_kinds[KIND_CACHE];
static unsigned g_kinds_next;

static int kind_uncached(const char *path, int depth) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return PROGRAM_TEXT;
    char head[160];
    ssize_t n = read(fd, head, sizeof head - 1);
    int kind = PROGRAM_TEXT;
    if (n >= 2 && head[0] == '#' && head[1] == '!') {
        /* A script is what its interpreter is: one run by this shell speaks
         * the wire (its statements emit frames when fd 3 is a pipe), one run
         * by sh is text. */
        head[n] = 0;
        char interp[160];
        size_t i = 2, k = 0;
        while (head[i] == ' ' || head[i] == '\t') i++;
        while (head[i] && head[i] != '\n' && head[i] != ' ' && head[i] != '\t' && head[i] != '\r' && k < sizeof interp - 1)
            interp[k++] = head[i++];
        interp[k] = 0;
        kind = depth == 0 && interp[0] == '/' && kind_uncached(interp, 1) == PROGRAM_WIRE ? PROGRAM_SCRIPT_WIRE
                                                                                         : PROGRAM_SCRIPT;
    } else if (elf_has_section(fd, SVAL_WIRE_SECTION)) {
        kind = PROGRAM_WIRE;
    }
    close(fd);
    return kind;
}

int shell_program_kind(const char *path) {
    for (unsigned i = 0; i < KIND_CACHE; i++)
        if (g_kinds[i].path[0] && !strcmp(g_kinds[i].path, path)) return g_kinds[i].kind;
    int kind = kind_uncached(path, 0);
    unsigned slot = g_kinds_next++ % KIND_CACHE;
    snprintf(g_kinds[slot].path, sizeof g_kinds[slot].path, "%s", path);
    g_kinds[slot].kind = kind;
    return kind;
}

static bool speaks_wire(int kind) { return kind == PROGRAM_WIRE || kind == PROGRAM_SCRIPT_WIRE; }

/* ==========================================================================
 * Arguments
 * ========================================================================== */
struct args {
    char  *argv[ARGV_MAX + 2];
    int    argc;
    char  *mem;              /* the strings, end to end */
    size_t used, cap;
    size_t bytes;            /* what the kernel counts: each string and its NUL */
};

static int args_push(struct args *a, const char *s, size_t n, char *err, size_t errcap, const char *who, size_t which) {
    if (memchr(s, 0, n)) {
        snprintf(err, errcap, "%s: argument %zu contains a NUL byte, which no program can be given", who, which);
        return -1;
    }
    if (a->argc >= ARGV_MAX) {
        snprintf(err, errcap, "%s: more than %d arguments -- a program can be given at most %d", who,
                 ARGV_MAX - 1, ARGV_MAX - 1);
        return -1;
    }
    if (a->used + n + 1 > a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 1024;
        while (nc < a->used + n + 1) nc *= 2;
        char *nm = (char *)realloc(a->mem, nc);
        if (!nm) { snprintf(err, errcap, "out of memory"); return -1; }
        /* the strings moved: re-point every argv entry at its new place */
        for (int i = 0; i < a->argc; i++) a->argv[i] = nm + (a->argv[i] - a->mem);
        a->mem = nm; a->cap = nc;
    }
    memcpy(a->mem + a->used, s, n);
    a->mem[a->used + n] = 0;
    a->argv[a->argc++] = a->mem + a->used;
    a->used += n + 1;
    a->bytes += n + 1;
    return 0;
}

/* One evaluated argument: text and numbers as they print; a LIST is one
 * argument per item; a TABLE (`glob`, `ls`) is one per row, from its `path`
 * column or else its `name`; null is none. A record has no one obvious text. */
static int args_value(struct args *a, const struct value *v, char *err, size_t errcap, const char *who, size_t which) {
    char small[96];
    switch (v->type) {
    case VAL_NULL:
        return 0;
    case VAL_STRING:
    case VAL_PATH:
        return args_push(a, v->u.s.bytes, v->u.s.len, err, errcap, who, which);
    case VAL_LIST:
        for (size_t i = 0; i < v->u.list.count; i++) {
            const struct value *it = &v->u.list.items[i];
            if (it->type == VAL_LIST || it->type == VAL_TABLE || it->type == VAL_RECORD) {
                snprintf(err, errcap, "%s: argument %zu is a list holding a %s; pass its items as text", who, which,
                         it->type == VAL_RECORD ? "record" : it->type == VAL_TABLE ? "table" : "list");
                return -1;
            }
            if (args_value(a, it, err, errcap, who, which)) return -1;
        }
        return 0;
    case VAL_TABLE: {
        const struct table *t = v->u.table;
        if (!t || !t->count) return 0;
        const char *col = record_field(&t->rows[0], "path") ? "path"
                        : record_field(&t->rows[0], "name") ? "name" : NULL;
        if (!col) {
            snprintf(err, errcap, "%s: argument %zu is a table without a path or name column", who, which);
            return -1;
        }
        for (size_t i = 0; i < t->count; i++) {
            const struct value *cell = record_field(&t->rows[i], col);
            if (cell && args_value(a, cell, err, errcap, who, which)) return -1;
        }
        return 0;
    }
    case VAL_RECORD:
        snprintf(err, errcap, "%s: argument %zu is a record; pass one of its fields", who, which);
        return -1;
    default: {
        size_t n = sval_format_scalar(v, small, sizeof small);
        return args_push(a, small, n, err, errcap, who, which);
    }
    }
}

/* argv for `cmd`, every argument evaluated -- or the error saying which limit
 * it would break. */
static struct value args_build(struct args *a, const struct command *cmd, const char *path, struct scope *env) {
    char err[200];
    memset(a, 0, sizeof *a);
    if (args_push(a, path, strlen(path), err, sizeof err, cmd->name, 0)) return value_error(err);
    for (size_t i = 0; i < cmd->nargs; i++) {
        struct value v = expr_eval(cmd->args[i], env);
        if (v.type == VAL_ERROR) return v;
        int rc = args_value(a, &v, err, sizeof err, cmd->name, i + 1);
        value_free(&v);
        if (rc) return value_error(err);
    }
    a->argv[a->argc] = NULL;
    size_t envc = 0, env_bytes = 0;
    for (char **e = environ; e && *e; e++) { envc++; env_bytes += strlen(*e) + 1; }
    size_t need = ROOM(a->bytes, a->argc) + ROOM(env_bytes, envc);
    if (need > ARG_PAGE) {
        snprintf(err, sizeof err,
                 "%s: the arguments (%zu bytes in %d) and the environment (%zu bytes) need %zu bytes; a program "
                 "starts with %u for all of them", cmd->name, a->bytes, a->argc, env_bytes, need, ARG_PAGE);
        return value_error(err);
    }
    return value_null();
}

static void args_free(struct args *a) { free(a->mem); a->mem = NULL; }

/* ==========================================================================
 * Redirections
 * ========================================================================== */
/* The file a redirection names, absolute. A bare word is the path as written;
 * anything else is evaluated and must come out as text. */
static struct value redirect_path(const struct redirect *r, struct scope *env, char *out, size_t cap) {
    const char *word = expr_as_word(r->target);
    char given[256];
    if (word) {
        snprintf(given, sizeof given, "%s", word);
    } else {
        struct value v = expr_eval(r->target, env);
        if (v.type == VAL_ERROR) return v;
        if (v.type != VAL_STRING && v.type != VAL_PATH) {
            value_free(&v);
            return value_error("a redirection needs a file name");
        }
        snprintf(given, sizeof given, "%.*s", (int)v.u.s.len, v.u.s.bytes);
        value_free(&v);
    }
    if (!given[0]) return value_error("a redirection needs a file name");
    shell_path_resolve(given, out, cap);
    return value_null();
}

static const struct redirect *redirect_find(const struct command *cmd, int a, int b, int c, int d) {
    for (size_t i = 0; i < cmd->nredirs; i++) {
        int k = cmd->redirs[i].kind;
        if (k == a || k == b || k == c || k == d) return &cmd->redirs[i];
    }
    return NULL;
}
static const struct redirect *redirect_out(const struct command *cmd) {
    return redirect_find(cmd, REDIR_OUT, REDIR_OUT_APPEND, REDIR_BOTH, REDIR_BOTH_APPEND);
}
static const struct redirect *redirect_err(const struct command *cmd) {
    return redirect_find(cmd, REDIR_ERR, REDIR_ERR_APPEND, REDIR_BOTH, REDIR_BOTH_APPEND);
}
static const struct redirect *redirect_in(const struct command *cmd) {
    return redirect_find(cmd, REDIR_IN, -1, -1, -1);
}
static bool appends(int kind) {
    return kind == REDIR_OUT_APPEND || kind == REDIR_ERR_APPEND || kind == REDIR_BOTH_APPEND;
}

/* Bytes to a file, replacing or appending. */
static struct value write_file(const char *path, bool append, const void *data, size_t n, const char *who) {
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    char msg[200];
    if (fd < 0) {
        snprintf(msg, sizeof msg, "%s: can't write '%s'", who, path);
        return value_error(msg);
    }
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) {
            close(fd);
            snprintf(msg, sizeof msg, "%s: writing '%s' failed", who, path);
            return value_error(msg);
        }
        p += w; n -= (size_t)w;
    }
    close(fd);
    return value_null();
}

struct value stage_redirect_input(const struct command *cmd, struct value input, struct scope *env) {
    const struct redirect *in = redirect_in(cmd);
    if (!in) return input;
    if (input.type != VAL_NULL) {
        value_free(&input);
        char msg[160];
        snprintf(msg, sizeof msg, "%s: has piped input and in< both", cmd->name);
        return value_error(msg);
    }
    char path[256];
    struct value e = redirect_path(in, env, path, sizeof path);
    if (e.type == VAL_ERROR) return e;
    return shell_read_file(path, cmd->name);
}

struct value stage_redirect_output(const struct command *cmd, struct value result, struct scope *env) {
    const struct redirect *out = redirect_out(cmd), *err = redirect_err(cmd);
    char path[256];
    if (result.type == VAL_ERROR) {
        if (!err) return result;
        struct value e = redirect_path(err, env, path, sizeof path);
        if (e.type == VAL_ERROR) { value_free(&result); return e; }
        char line[300];
        int n = snprintf(line, sizeof line, "%s: %s\n", cmd->name, value_error_msg(&result));
        value_free(&result);
        return write_file(path, appends(err->kind), line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1),
                          cmd->name);
    }
    if (!out) return result;
    struct value e = redirect_path(out, env, path, sizeof path);
    if (e.type == VAL_ERROR) { value_free(&result); return e; }
    struct wire_buf text;
    wire_buf_init(&text);
    int rc = result.type == VAL_NULL ? 0 : sval_render(&result, &text);
    value_free(&result);
    if (rc) { wire_buf_free(&text); return value_error("out of memory"); }
    struct value w = write_file(path, appends(out->kind), text.data, text.len, cmd->name);
    wire_buf_free(&text);
    return w;
}

/* ==========================================================================
 * One external stage
 * ========================================================================== */
static struct value spawn_error(const struct command *cmd, const char *path, int64_t rc) {
    char msg[200];
    if (-rc == EXT_ENOENT) snprintf(msg, sizeof msg, "%s: '%s' is gone", cmd->name, path);
    else if (-rc == EXT_ENOEXEC) snprintf(msg, sizeof msg, "%s: '%s' is not a program this system runs "
                                                           "(not ELF/EMBX, or a script whose interpreter is a script)",
                                         cmd->name, path);
    else snprintf(msg, sizeof msg, "%s: can't run (err %d)", cmd->name, (int)-rc);
    return value_error(msg);
}

struct value extern_stage_run(const struct command *cmd, struct value input,
                              struct scope *env, bool to_terminal) {
    char msg[200];

    /* --- the program, and what it speaks --- */
    char path[256];
    if (shell_resolve_program(cmd->name, path, sizeof path) != 0) {
        value_free(&input);
        if (strchr(cmd->name, '/')) snprintf(msg, sizeof msg, "%s: no such program ('%s')", cmd->name, path);
        else snprintf(msg, sizeof msg, "%s: no such command (not a builtin, not in /system/bin or /data/apps)",
                      cmd->name);
        return value_error(msg);
    }
    bool wire = speaks_wire(shell_program_kind(path));

    /* --- redirections --- */
    const struct redirect *r_in = redirect_in(cmd), *r_out = redirect_out(cmd), *r_err = redirect_err(cmd);
    char in_path[256] = "", out_path[256] = "", err_path[256] = "";
    struct value e;
    if (r_in && (e = redirect_path(r_in, env, in_path, sizeof in_path)).type == VAL_ERROR) { value_free(&input); return e; }
    if (r_out && (e = redirect_path(r_out, env, out_path, sizeof out_path)).type == VAL_ERROR) { value_free(&input); return e; }
    if (r_err && (e = redirect_path(r_err, env, err_path, sizeof err_path)).type == VAL_ERROR) { value_free(&input); return e; }
    if (r_in && input.type != VAL_NULL) {
        value_free(&input);
        snprintf(msg, sizeof msg, "%s: has piped input and in< both", cmd->name);
        return value_error(msg);
    }

    /* --- argv: every argument, within the kernel's limits --- */
    struct args args;
    e = args_build(&args, cmd, path, env);
    if (e.type == VAL_ERROR) { args_free(&args); value_free(&input); return e; }

    /* --- how its output comes back ---
     * structured: frames on fd 3, unless its output goes to a file;
     * text: stdout through a pipe, unless it goes to a file or the terminal. */
    bool collect_wire = wire && !r_out;
    bool collect_text = !wire && !r_out && !to_terminal;

    /* --- its input, ready to stream: frames for a structured program, the
     *     text a person would see for a text one --- */
    bool has_input = input.type != VAL_NULL;
    struct wire_buf inframe;
    wire_buf_init(&inframe);
    if (has_input && (wire ? wire_serialize(&input, &inframe) : sval_render(&input, &inframe)) != 0) {
        value_free(&input);
        wire_buf_free(&inframe);
        args_free(&args);
        return value_error("out of memory preparing pipeline input");
    }
    value_free(&input);

    struct embk_spawn_file_action acts[6];
    memset(acts, 0, sizeof acts);
    int nact = 0;

    /* --- pipe A: what comes back (fd 3 structured, or fd 1 text) --- */
    int pa[2] = { -1, -1 };
    if (collect_wire || collect_text) {
        if (embk_pipe(pa) != 0) { wire_buf_free(&inframe); args_free(&args); return value_error("pipe failed"); }
        acts[nact].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;
        acts[nact].target_fd = collect_wire ? SVAL_FD_OUT : 1;
        acts[nact].src_obj_handle = pa[1];
        nact++;
    }

    /* --- pipe B: the input (child fd 0), only when there IS input --- */
    int pb[2] = { -1, -1 };
    if (has_input) {
        if (embk_pipe(pb) != 0) {
            if (pa[0] >= 0) { embk_close_handle(pa[0]); embk_close_handle(pa[1]); }
            wire_buf_free(&inframe);
            args_free(&args);
            return value_error("pipe failed");
        }
        acts[nact].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;
        acts[nact].target_fd = SVAL_FD_IN;
        acts[nact].src_obj_handle = pb[0];
        nact++;
    }

    /* --- files on fds 0-2 --- */
    if (r_in) {
        acts[nact].kind = EMBK_SPAWN_ACTION_OPEN;
        acts[nact].target_fd = 0;
        snprintf(acts[nact].path, sizeof acts[nact].path, "%s", in_path);
        acts[nact].flags = EMBK_O_RDONLY;
        nact++;
    }
    if (r_out) {
        acts[nact].kind = EMBK_SPAWN_ACTION_OPEN;
        acts[nact].target_fd = 1;
        snprintf(acts[nact].path, sizeof acts[nact].path, "%s", out_path);
        acts[nact].flags = EMBK_O_WRONLY | EMBK_O_CREAT | (appends(r_out->kind) ? EMBK_O_APPEND : EMBK_O_TRUNC);
        acts[nact].mode = 0644;
        nact++;
    }
    if (r_err && r_err == r_out) {                       /* out+err>: one file, both streams */
        acts[nact].kind = EMBK_SPAWN_ACTION_DUP2;
        acts[nact].src_obj_handle = 1;
        acts[nact].target_fd = 2;
        nact++;
    } else if (r_err) {
        acts[nact].kind = EMBK_SPAWN_ACTION_OPEN;
        acts[nact].target_fd = 2;
        snprintf(acts[nact].path, sizeof acts[nact].path, "%s", err_path);
        acts[nact].flags = EMBK_O_WRONLY | EMBK_O_CREAT | (appends(r_err->kind) ? EMBK_O_APPEND : EMBK_O_TRUNC);
        acts[nact].mode = 0644;
        nact++;
    }

    shell_tty_suspend();

    /* --- spawn ---
     * The child gets THE SHELL'S OWN environment, and gets it because this line
     * names it. Nothing is inherited on EmbLink: a child receives only what its
     * parent passes (see the kernel's spawn.h), so `environ` here is a decision,
     * visible at the call site, exactly like the file-actions above. Edit it
     * with the `env` builtin; inspect it with plain `env`. */
    int64_t child = embk_spawn_env(path, args.argv, environ, acts, nact);
    args_free(&args);
    if (child >= 0) {
        /* HAND OVER the right to be interrupted for as long as this command runs
         * (docs/INTERRUPTION.md). A ^C now cancels the CHILD, not this shell.
         *
         * This is the delegation the whole design turns on: EmbLink has no
         * foreground process to infer, so the shell -- which holds the console --
         * simply says who ^C means while it is not the one running. Reclaimed
         * unconditionally after the wait below; leaving it pointing at a reaped
         * child would make the NEXT ^C silently do nothing. */
        shell_interrupt_route((int)child);
    }
    if (child < 0) {
        if (pa[0] >= 0) { embk_close_handle(pa[0]); embk_close_handle(pa[1]); }
        if (has_input) { embk_close_handle(pb[0]); embk_close_handle(pb[1]); }
        wire_buf_free(&inframe);
        shell_tty_resume();   /* suspend() ran BEFORE the spawn, so undo it even
                               * when the child never came to exist */
        return spawn_error(cmd, path, child);
    }

    /* --- drop OUR copies of the ends the child now holds (EOF depends on it) --- */
    bool out_open = pa[0] >= 0;
    if (out_open) embk_close_handle(pa[1]);
    if (has_input) embk_close_handle(pb[0]);

    /* --- our own ends become plain fds --- */
    if (out_open && embk_fd_install_obj(pa[0], EXT_FD_OUTPUT_READ) < 0) {
        embk_close_handle(pa[0]);
        if (has_input) { embk_close_handle(pb[1]); }
        wire_buf_free(&inframe);
        shell_tty_resume();
        embk_wait((int)child);
        return value_error("fd install failed");
    }
    if (out_open) embk_close_handle(pa[0]);
    bool in_open = false;
    if (has_input) {
        if (embk_fd_install_obj(pb[1], EXT_FD_INPUT_WRITE) < 0) {
            embk_close_handle(pb[1]);
            if (out_open) close(EXT_FD_OUTPUT_READ);   /* an FD (self-installed above), not a handle */
            wire_buf_free(&inframe);
            shell_tty_resume();
            embk_wait((int)child);
            return value_error("fd install failed");
        }
        embk_close_handle(pb[1]);
        in_open = true;
    }

    /* --- the STREAMING pump --- */
    struct sval_reader rd;
    sval_reader_init(&rd);
    struct value collected = value_null();
    size_t nframes = 0;
    struct wire_buf text;                                 /* a text program's stdout */
    wire_buf_init(&text);
    size_t off = 0;
    int rc = 0;
    bool too_much = false;
    struct cancel_watch cw = {0, false};

    /* What the output pipe has right now, into frames or text. 1 if anything
     * arrived, 0 if nothing, -1 on a protocol error or too much text. */
#define DRAIN_SOME()                                                                     \
    ({  int got_any = 0;                                                                 \
        for (;;) {                                                                       \
            int64_t nav = embk_fd_avail(EXT_FD_OUTPUT_READ);                             \
            if (nav <= 0) break;                                                         \
            uint8_t tmp[512];                                                            \
            size_t want = nav > (int64_t)sizeof tmp ? sizeof tmp : (size_t)nav;          \
            int64_t got = embk_read(EXT_FD_OUTPUT_READ, tmp, want);                      \
            if (got <= 0) break;                                                         \
            got_any = 1;                                                                 \
            if (collect_wire) {                                                          \
                if (sval_reader_feed(&rd, tmp, (size_t)got) != 0) { got_any = -1; break; } \
                struct value v;                                                          \
                int prc;                                                                 \
                while ((prc = sval_reader_next(&rd, &v)) == 1)                           \
                    absorb_frame(&collected, &nframes, v);                               \
                if (prc < 0) { got_any = -1; break; }                                    \
            } else {                                                                     \
                if (text.len + (size_t)got > TEXT_CAPTURE_MAX) { too_much = true; got_any = -1; break; } \
                if (text.len + (size_t)got > text.cap) {                                 \
                    size_t nc = text.cap ? text.cap * 2 : 4096;                          \
                    while (nc < text.len + (size_t)got) nc *= 2;                         \
                    uint8_t *nd = (uint8_t *)realloc(text.data, nc);                     \
                    if (!nd) { got_any = -1; break; }                                    \
                    text.data = nd; text.cap = nc;                                       \
                }                                                                        \
                memcpy(text.data + text.len, tmp, (size_t)got);                          \
                text.len += (size_t)got;                                                 \
            }                                                                            \
        }                                                                                \
        got_any; })

    while (in_open && rc == 0) {
        bool progress = false;

        int64_t space = embk_fd_avail(EXT_FD_INPUT_WRITE);
        if (space > 0 && off < inframe.len) {
            size_t chunk = inframe.len - off;
            if (chunk > (size_t)space) chunk = (size_t)space;
            int64_t w = embk_write(EXT_FD_INPUT_WRITE, inframe.data + off, chunk);
            if (w > 0) { off += (size_t)w; progress = true; }
            else if (w == -EXT_EPIPE) off = inframe.len;   /* child hung up stdin
                                                            * early -- legal; stop */
            /* any OTHER w <= 0: a transient fault -- leave `off` put and fall
             * through to yield + retry. Truncating here would hand the child a
             * partial frame ("corrupt structured input") -- the flaky bug this
             * replaced, which fired only under scheduler pressure. */
        }
        if (off >= inframe.len) {
            /* EOF to the child. close(), NOT embk_close_handle():
             * EXT_FD_INPUT_WRITE is an FD (self-installed via fd_install_obj;
             * its handle was closed right after installing). */
            close(EXT_FD_INPUT_WRITE);
            in_open = false;
            progress = true;
        }

        if (out_open) {
            int d = DRAIN_SOME();
            if (d < 0) rc = -1;
            else if (d > 0) progress = true;
        }

        if (!progress) {
            /* Both pipes momentarily stuck: the child is computing (or
             * declining a cancellation). Run the escalation policy, then give
             * it the CPU. */
            cancel_watch_tick(&cw, child);
            embk_yield();
        }
    }
    if (in_open) close(EXT_FD_INPUT_WRITE);      /* error exit mid-feed (an FD, see above) */
    wire_buf_free(&inframe);

    /* --- stdin done: drain the output to EOF, or wait for the child ---
     * Poll rather than block: for a command with no piped input the shell
     * spends the child's ENTIRE runtime here, and a ^C'd child that declines
     * to exit must still meet the grace-then-kill policy. Once the child is
     * gone its fds are closed (fds are released AT EXIT, "a zombie holds no
     * fds"), so the last drain cannot hang; draining even after a kill is
     * deliberate -- output the child completed before dying is real. */
    if (rc == 0) {
        for (;;) {
            if (out_open) {
                int d = DRAIN_SOME();
                if (d < 0) { rc = -1; break; }
                if (d > 0) continue;
            }
            if (!embk_proc_alive((int)child)) {
                if (out_open) {
                    if (collect_wire) {
                        struct value v;
                        int brc;
                        while ((brc = sval_read_blocking(&rd, EXT_FD_OUTPUT_READ, &v)) == 1)
                            absorb_frame(&collected, &nframes, v);
                        if (brc < 0) rc = -1;
                    } else {
                        while (DRAIN_SOME() > 0) { }
                        if (too_much) rc = -1;
                    }
                }
                break;
            }
            cancel_watch_tick(&cw, child);
            /* 10ms, not yield: this loop can run for the child's whole life,
             * and a yield-spin would burn a core against it. 10ms is invisible
             * next to human-scale output and the 1500ms grace. */
            embk_sleep_ms(10);
        }
    }
#undef DRAIN_SOME
    sval_reader_free(&rd);
    if (out_open) close(EXT_FD_OUTPUT_READ);

    int64_t code = embk_wait((int)child);
    if (cw.killed || embk_child_cancelled((int)child) == 1) exec_interrupt_note();   /* ^C ended it: 130 */

    /* Take the console back: from here on ^C means THIS shell again. Done
     * unconditionally -- a route left pointing at a reaped child makes the next
     * ^C silently do nothing, which reads as a bug rather than a stale route. */
    shell_interrupt_route(-1);

    /* Console back in the shell's hands -> its own line editing resumes. */
    shell_tty_resume();

    /* ^Z: IT STOPPED RATHER THAN FINISHED.
     *
     * The child is still there, frozen, holding its memory and its fds, and
     * this handle is still the only name anyone has for it -- so it becomes a
     * JOB, which is exactly what ^Z means. Dropping the handle here would
     * strand a live process nothing could ever name again.
     *
     * Whatever it had already written is kept and returned: it is real output
     * that really happened, and throwing it away because the command was
     * paused afterwards would lose data on a keystroke. */
    if (code == -EMBK_ESTOPPED) {
        int id = jobs_adopt_stopped((int)child, cmd->name);
        if (id < 0) {
            /* The table is full and there is nowhere to remember it. Letting it
             * run on unnamed would leak a process no one can reach, so it is
             * resumed and left to finish on its own -- the honest recovery,
             * and it is reported rather than hidden. */
            (void)embk_resume((int)child);
            (void)embk_wait((int)child);
            value_free(&collected);
            wire_buf_free(&text);
            snprintf(msg, sizeof msg, "%s: stopped, but the job table is full -- resumed it", cmd->name);
            return value_error(msg);
        }
        char note[96];
        snprintf(note, sizeof note, "[%d] stopped  %s\n", id, cmd->name);
        embk_puts(1, note);
        if (collect_text) {
            value_free(&collected);
            collected = value_string_n((const char *)text.data, text.len);
        }
        wire_buf_free(&text);
        return collected;
    }

    if (rc < 0) {
        value_free(&collected);
        wire_buf_free(&text);
        if (too_much) snprintf(msg, sizeof msg, "%s: printed more than 16 MB into the pipeline", cmd->name);
        else snprintf(msg, sizeof msg, "%s: corrupt structured output", cmd->name);
        return value_error(msg);
    }
    if (collect_text) {
        /* A text program's result is what it printed, whatever its exit --
         * output it produced is real -- and an error only when it failed
         * having printed nothing. */
        if (text.len == 0 && code != 0) {
            wire_buf_free(&text);
            snprintf(msg, sizeof msg, "%s: exited with code %lld", cmd->name, (long long)code);
            return value_error(msg);
        }
        struct value s = value_string_n((const char *)text.data, text.len);
        wire_buf_free(&text);
        return s.type == VAL_NULL && text.len ? value_error("out of memory") : s;
    }
    wire_buf_free(&text);
    if (nframes == 0 && code != 0) {
        /* no structured output AND a failing exit: surface the failure */
        snprintf(msg, sizeof msg, "%s: exited with code %lld", cmd->name, (long long)code);
        return value_error(msg);
    }
    return collected;   /* 0 frames -> null, 1 -> the value, n -> a list */
}
