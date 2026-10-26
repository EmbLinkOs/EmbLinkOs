/* ==========================================================================
 * main.c -- the EmbLink shell driver (shell.elf). Ties the stages together:
 * read a line -> lex -> parse -> evaluate -> sink.
 *
 * Two modes:
 *   shell.elf              interactive REPL on fds 0/1 (the console)
 *   shell.elf -c "line" [arg...]   run ONE line (args are $args), exit 0 / 1
 *                          (what `test shell` and scripts drive)
 *
 * The SINK (the sketch's last box): if fd 3 is a structured pipe
 * (sval_structured_out -- the shell itself was composed into a bigger
 * pipeline), EMIT the result as a frame; otherwise pretty-print to fd 1.
 *
 * The interactive line is the shell's own (shell/edit): on a terminal it
 * draws the prompt, edits, completes (shell/complete) and recalls history
 * itself, in raw mode; on the kernel console, which parses no escapes, the
 * same editor works with backspaces. The prompt is framed in OSC 133 marks
 * (A with edit=shell, B, C, D;status) so the terminal knows where prompts and
 * commands begin and that it must not edit the line itself.
 * ========================================================================== */
#include "lex/lex.h"
#include "parse/parse.h"
#include "eval/eval.h"
#include "eval/exec.h"
#include "sval/sval.h"
#include "hist/hist.h"
#include "edit/edit.h"
#include "complete/complete.h"
#include "complete/highlight.h"
#include "prompt/prompt.h"
#include "prompt/prompt_os.h"
#include "prompt/wizard_os.h"
#include "embk.h"
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>


#define LINE_MAX_LEN 512

/* True when THIS shell's stdin is the machine console and we've put it in raw mode
 * -- i.e. we own the console session. False under the Terminal app or '-c'
 * (stdin is a pipe, not the console): then the global console mode is someone
 * else's property and we must never touch it. same fstat-shape probe as sval.c's
 * fd_is_fifo, for the same reason: ride the standard stat shape the newlib stub 
 *already fills. */
static int g_console_session = 0;
static int g_terminal_session = 0;
static struct termios g_prompt_attr;

void shell_interrupt_route(int handle) {
    if (g_terminal_session) (void)embk_terminal_foreground(0,handle);
    else if (g_console_session) (void)embk_console_interrupt_route(handle);
}

static bool stdin_is_console(void) {
    struct stat st;
    if (fstat(0, &st) != 0) return false;
    return S_ISCHR(st.st_mode);                         /* the console fstats as a character device */
}

/* The termions dance, EmbLink edition. Suspend = hand the line discipline back to
 * the tty (cooked) for the lifetime of a foreground child, so a child that reads
 * stdin gets echo/editing/^D-EOF from the kernel, knowing nothing about this shell.
 * Resume = take it back for our own editor. Both are no-ops unless we own the session,
 * so eval_extern can call them unconditionally. */
void shell_tty_suspend(void) {
    if (g_console_session) embk_tty_mode(EMBK_TTY_COOKED);
    if (g_terminal_session) {
        struct termios child=g_prompt_attr; child.c_lflag|=ICANON|ECHO|ISIG;
        (void)tcsetattr(0,TCSANOW,&child);
    }
}

void shell_tty_resume(void) {
    if (g_console_session) embk_tty_mode(EMBK_TTY_RAW);
    if (g_terminal_session) {
        struct termios active=g_prompt_attr; active.c_lflag|=ISIG;
        (void)tcsetattr(0,TCSANOW,&active);
        shell_interrupt_route(EMBK_TERMINAL_SELF);
    }
}

static void puts1(const char *s) { write(1, s, strlen(s)); }

/* -------------------------------------------------------------------------
 * THE SINK -- what happens to each statement's value.
 *
 * The executor must not know whether a human is watching: a `for` loop's body
 * inside a script and a line typed at the prompt take the identical path, and
 * the only difference is where the result goes. If this shell was composed
 * into a bigger pipeline (fd 3 is a structured pipe) it EMITS a frame;
 * otherwise it pretty-prints.
 * ------------------------------------------------------------------------- */
static void print_sink(struct value *v, void *ctx) {
    (void)ctx;
    if (sval_structured_out())
        (void)sval_emit(v);
    else
        (void)sval_print(v, 1);
}

/* -------------------------------------------------------------------------
 * Run a PROGRAM -- one typed line, or a whole script file. Returns 0 on
 * success, 1 on any error (already printed).
 *
 * A parsed program whose statements include a `def` is HANDED to the executor
 * rather than freed: a definition stores a borrowed pointer into this AST, so
 * freeing it here would leave the command table pointing at freed memory the
 * moment the line that defined it returned. exec_retain_program() says so out
 * loud instead of leaving it to a comment.
 * ------------------------------------------------------------------------- */
static bool block_defines_a_command(const struct block *b) {
    for (size_t i = 0; i < b->n; i++) {
        if (b->stmts[i]->kind == STMT_DEF) return true;
        /* A `def` nested inside `if`/`while`/`for` is legal and reaches the
         * table the same way, so the whole AST has to be retained for it too.
         * Rather than walk every arm, treat any control-flow statement as
         * possibly containing one: the cost of being wrong in this direction
         * is one retained AST, and in the other it is a use-after-free. */
        switch (b->stmts[i]->kind) {
        case STMT_IF: case STMT_WHILE: case STMT_FOR: return true;
        default: break;
        }
    }
    return false;
}

/* --- being interrupted ----------------------------------------------------
 *
 * While a program runs, ^C is AIMED AT THIS SHELL and caught rather than
 * cancelling it (embk_intr_catch). At the prompt it is handed back to nobody,
 * where the kernel delivers it as the byte 0x03 and the line editor treats it as a
 * line-edit key -- which is what a person expects an empty prompt's ^C to do.
 *
 * Routing it at SELF used to be impossible: ^C set the sticky cancellation
 * flag, so one keystroke ended the shell for good. The kernel now has a
 * counted, clearable interrupt channel alongside cancellation precisely
 * because a shell needs to survive being interrupted.
 *
 * eval_extern hands the route to a CHILD for the child's lifetime and takes it
 * back after -- there, ^C IS a real cancellation, which is right: interrupting
 * `wget` should stop wget. This only covers the gap the child does not fill:
 * the shell's own loops and the spaces between commands. */
static bool shell_interrupted(void) {
    return embk_intr_take() != 0;
}

static int run_program(const char *src, struct scope *top) {
    size_t ntoks = 0;
    struct token *toks = lex(src, &ntoks);
    if (!toks) { puts1("error: out of memory\n"); return 1; }

    char err[160];
    struct block *prog = parse_program(toks, ntoks, err, sizeof err);
    lex_free_tokens(toks, ntoks);
    if (!prog) {
        puts1("error: ");
        puts1(err);
        puts1("\n");
        return 1;
    }
    if (prog->n == 0) { block_free(prog); return 0; }   /* blank input */

    /* Catch our own command loops on the actual terminal session. A pipe
     * does not imply ownership of the machine console's interrupt route. */
    if (g_console_session || g_terminal_session) {
        (void)embk_intr_take();                 /* drop anything stale */
        (void)embk_intr_catch(1);
        shell_interrupt_route(EMBK_INTR_ROUTE_SELF);
        if (g_terminal_session) {
            struct termios active=g_prompt_attr; active.c_lflag|=ISIG;
            (void)tcsetattr(0,TCSANOW,&active);
        }
    }

    struct exec_out out = { FLOW_NORMAL, value_null(), false };
    (void)exec_interrupt_hit(true);
    int rc = block_exec(prog, top, print_sink, NULL, &out);
    bool interrupted = exec_interrupt_hit(true);

    if (g_console_session || g_terminal_session) {
        shell_interrupt_route(-1);
        if (g_terminal_session) (void)tcsetattr(0,TCSANOW,&g_prompt_attr);
        (void)embk_intr_catch(0);
    }

    if (rc != 0) {
        puts1("error: ");
        puts1(out.value.type == VAL_ERROR ? value_error_msg(&out.value)
                                          : "command failed");
        puts1("\n");
    } else if (out.flow == FLOW_BREAK || out.flow == FLOW_CONTINUE) {
        /* Reaching the top still unwinding means there was no loop to unwind
         * to. Silence here would make a misplaced `break` look like it worked. */
        puts1("error: ");
        puts1(out.flow == FLOW_BREAK ? "'break' outside a loop\n"
                                     : "'continue' outside a loop\n");
        rc = -1;
    }
    value_free(&out.value);

    if (block_defines_a_command(prog)) {
        if (!exec_retain_program(prog)) {
            puts1("error: too many definitions in this session\n");
            rc = -1;
        }
        /* NOT freed -- the command table points into it. */
    } else {
        block_free(prog);
    }
    return interrupted ? 130 : rc != 0 ? 1 : 0;      /* ^C: 128 + SIGINT, as every shell has it */
}

/* One typed line. Kept as its own name because that is what the REPL, `-c`
 * and the completion probe all mean; a line is simply the shortest program. */
static int run_line(const char *line, struct scope *top) {
    return run_program(line, top);
}

/* -------------------------------------------------------------------------
 * Run a SCRIPT FILE. The whole file is read and parsed as ONE program, not
 * line by line: a block spans lines, and a line-at-a-time reader cannot see
 * the end of one.
 *
 * A leading `#!` line is skipped rather than parsed. It is a comment to the
 * lexer anyway (`#` already starts one), so this is belt and braces -- but it
 * means a script that names an interpreter reads correctly whether or not the
 * kernel ever learns to honour it.
 * ------------------------------------------------------------------------- */
static int run_script(const char *path, struct scope *top) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        puts1("shell: cannot open ");
        puts1(path);
        puts1("\n");
        return 1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); puts1("shell: cannot size the script\n"); return 1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); puts1("shell: cannot size the script\n"); return 1; }
    rewind(f);

    char *src = (char *)malloc((size_t)n + 1);
    if (!src) { fclose(f); puts1("shell: out of memory\n"); return 1; }
    size_t got = fread(src, 1, (size_t)n, f);
    fclose(f);
    src[got] = '\0';

    int rc = run_program(src, top);
    free(src);
    return rc;
}

/* A script's arguments, as the list $args: `./build.esh fast` (its #! line
 * runs `shell.elf ./build.esh fast`) sees ["fast"]. Text, as they came. */
static void bind_args(struct scope *top, int argc, char **argv, int first) {
    struct value list = value_list();
    for (int i = first; i < argc; i++) value_list_push(&list, value_string(argv[i]));
    (void)scope_bind(top, "args", list);
}

/* -------------------------------------------------------------------------
 * The interactive line: the editor (shell/edit), fed from fd 0.
 * ------------------------------------------------------------------------- */
static struct editor *g_ed;
static char g_ahead[512];             /* keys typed past the end of the last line */
static size_t g_ahead_n;

static void out_write(void *ctx, const char *bytes, size_t n) {
    (void)ctx;
    while (n) {
        ssize_t w = write(1, bytes, n);
        if (w <= 0) return;
        bytes += w;
        n -= (size_t)w;
    }
}
static bool unfinished(void *ctx, const char *text) { (void)ctx; return program_incomplete(text); }

static int terminal_cols(void) {
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col) return ws.ws_col;
    return 80;
}

/* An ESC with nothing after it yet: wait a moment for the rest of a key's
 * sequence before deciding it was the Escape key itself. */
static bool input_within(int ms) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (embk_fd_poll(0, EMBK_POLLIN) & EMBK_POLLIN) return true;
        embk_sleep_ms(5);
    }
    return false;
}

static struct prompt_state g_pstate;      /* the prompt's state (below) */
static struct p10k_git g_git;

static enum edit_result edit_one_line(const struct edit_prompt *prompt, enum edit_mode mode) {
    edit_begin(g_ed, prompt, mode, mode == EDIT_VT ? terminal_cols() : 80);
    enum edit_result r = EDIT_MORE;
    if (g_ahead_n) {
        size_t used = 0;
        r = edit_feed(g_ed, g_ahead, g_ahead_n, &used);
        memmove(g_ahead, g_ahead + used, g_ahead_n - used);
        g_ahead_n -= used;
    }
    while (r == EDIT_MORE) {
        /* An answer the prompt is still waiting for (the git status): watch
         * for it beside the keys, and draw the prompt again when it comes. */
        while (prompt_os_git_waiting() >= 0 && !(embk_fd_poll(0, EMBK_POLLIN) & EMBK_POLLIN)) {
            if (prompt_os_git_answer(prompt_p10k_config(), &g_git)) {
                g_pstate.git = g_git.repo ? &g_git : NULL;
                prompt_os_fresh();
                edit_reprompt(g_ed);
            } else embk_sleep_ms(10);
        }
        char buf[256];
        ssize_t n = read(0, buf, sizeof buf);
        if (n <= 0) return EDIT_EOF;
        size_t used = 0;
        r = edit_feed(g_ed, buf, (size_t)n, &used);
        if (r != EDIT_MORE) {
            size_t rest = (size_t)n - used;
            if (rest > sizeof g_ahead - g_ahead_n) rest = sizeof g_ahead - g_ahead_n;
            memcpy(g_ahead + g_ahead_n, buf + used, rest);
            g_ahead_n += rest;
        }
        while (r == EDIT_MORE && edit_pending_escape(g_ed) && !input_within(40))
            r = edit_escape_timeout(g_ed);
    }
    return r;
}

/* A plain line from a pipe: no terminal on the other end, nothing to draw. */
static int read_plain_line(char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) <= 0) { buf[len] = '\0'; return len ? (int)len : -1; }
        if (c == '\n') break;
        if (c != '\r' && len + 1 < cap) buf[len++] = c;
    }
    buf[len] = '\0';
    return (int)len;
}

/* ~ for home, as every shell names where it stands. */
static void prompt_where(char *where, size_t cap) {
    char cwd[192];
    if (!getcwd(cwd, sizeof cwd)) { cwd[0] = '/'; cwd[1] = 0; }
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && strncmp(cwd, home, hl) == 0 && (cwd[hl] == '/' || cwd[hl] == 0))
        snprintf(where, cap, "~%s", cwd + hl);
    else
        snprintf(where, cap, "%s", cwd);
}

/* The prompt (shell/prompt): built from what the shell is doing now, and
 * built again for a new width (edit_host.reprompt) from the same state. */
static struct prompt_state g_pstate;
static struct prompt_text g_ptext;
static struct p10k_git g_git;

/* Background jobs that have not finished: Powerlevel10k's background_jobs. */
static int unfinished_jobs(void) {
    jobs_poll();
    int n = 0;
    for (size_t i = 0; i < jobs_count(); i++) {
        const struct job *j = jobs_at(i);
        if (j && j->id && !j->reaped) n++;
    }
    return n;
}
static void prompt_into(struct edit_prompt *p) {
    p->text = g_ptext.left;
    p->right = g_ptext.right[0] ? g_ptext.right : NULL;
    p->transient = g_ptext.transient[0] ? g_ptext.transient : NULL;
}
static void reprompt(void *ctx, int cols, struct edit_prompt *p) {
    (void)ctx;
    prompt_build(&g_pstate, cols, &g_ptext);
    prompt_into(p);
}

/* Whether directory `dir` lists `name`. Looking a missing file up costs a
 * line in the kernel's log, so the start of every shell looks first. */
static bool listed(const char *dir, const char *name) {
    static struct embk_dirent ents[64];
    int64_t n = embk_readdir(dir[0] ? dir : "/", ents, 64);
    for (int64_t i = 0; i < n; i++) if (!strcmp(ents[i].name, name)) return true;
    return false;
}

/* ~/.config/shell/prompt.esh, then rc.esh, for a session a person types
 * into: the prompt `prompt configure` wrote (it loads ~/.config/shell/p10k.zsh),
 * then the person's own options (prompt set ...) and whatever else they want
 * every time. Scripts and -c do not read them. Their errors are printed; the
 * shell starts anyway. */
static void source_rc(struct scope *top) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return;
    char base[200], config[224], shell[240], path[256];
    snprintf(base, sizeof base, "%s", strcmp(home, "/") ? home : "");
    snprintf(config, sizeof config, "%s/.config", base);
    snprintf(shell, sizeof shell, "%s/shell", config);
    if (!listed(base, ".config") || !listed(config, "shell")) return;
    static const char *const files[] = { "prompt.esh", "rc.esh" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        if (!listed(shell, files[i])) continue;
        snprintf(path, sizeof path, "%s/%s", shell, files[i]);
        (void)run_script(path, top);
    }
}

/* History lives in the user's home and is appended as lines are accepted, so
 * every Terminal tab and every session adds to the same one. */
static void open_history(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || !strcmp(home, "/")) return;
    char path[256];
    static const char *const dirs[] = { "/.local", "/.local/share", "/.local/share/shell" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, sizeof path, "%s%s", home, dirs[i]);
        (void)embk_mkdir(path);                 /* already there: fine */
    }
    snprintf(path, sizeof path, "%s/.local/share/shell/history", home);
    (void)hist_open(path);
}

/* Defaults this SHELL chooses for the commands it runs.
 *
 * Deliberately shell POLICY, not kernel behaviour. The kernel fabricates
 * nothing: a process gets an environment only because its spawner named one
 * (spawn.h), and a shell naming sensible defaults for its children is exactly
 * what a shell is for. Everything here is visible in `env` and overridable with
 * `env set`, so it is a stated default, not hidden magic.
 *
 * rewrite==0 IS THE POINT: if whoever spawned this shell already named HOME, we
 * must not clobber their choice with ours. Defaults lose to explicit decisions.
 *
 * HOME exists because tools look there for their config (git wants
 * ~/.gitconfig). PATH is "/" because that is where the flat EMBKFS root puts
 * every executable today. */
extern void shell_cwd_publish(void);   /* builtins_os.c */

static void seed_default_env(void) {
    setenv("HOME", "/", 0);
    setenv("PATH", "/", 0);
    /* Publish the directory we STARTED in (crt0 already seeded our cwd from an
     * inherited PWD, if our own spawner named one), so the first external
     * command runs where the user thinks it does -- not at "/". */
    shell_cwd_publish();
}

int main(int argc, char **argv) {
    struct scope top;
    scope_init(&top, NULL);
    seed_default_env();


    if (tcgetattr(0,&g_prompt_attr)==0) {
        g_terminal_session=1;
    } else if (stdin_is_console()) {
        embk_tty_mode(EMBK_TTY_RAW);
        /* At OUR prompt, ^C is a LINE-EDIT key (kill the input line), NOT a
         * cancellation -- so route it to NOBODY, which makes it arrive as the
         * byte 0x03 the line editor handles. Routing ^C to SELF was wrong: it
         * cancels this process, and the cancel flag is STICKY with no way to
         * clear it, so the shell could never read again -- one ^C exited it.
         * eval_extern hands the route to a CHILD for its lifetime (there ^C IS a
         * real cancellation) and reclaims it -- back to this NOBODY state -- after. */
        (void)embk_console_interrupt_route(-1);
        g_console_session = 1;
    }

    /* Colour is opt-IN by the display: the GUI terminal says what it is
     * (TERM=xterm-256color) and parses SGR; the serial console and plain
     * pipes never see an escape byte. The same contract Linux uses: TERM
     * decides, and an unset TERM or "dumb" means no escapes. */
    {
        const char *t = getenv("TERM");
        if (!g_console_session && t && t[0] && strcmp(t, "dumb") != 0)
            sval_set_colour(1);
    }

    /* The executor needs the session's top scope (user commands resolve
     * against it) and the sink their bodies print through, before ANY program
     * runs -- including the one-shot forms below. */
    exec_init(&top, print_sink, NULL);
    exec_set_interrupt_check(shell_interrupted);

    /* one-shot mode: shell.elf -c "ls | where size > 1mb" [arg...]
     * The argument is a whole PROGRAM, so `-c 'if x { a } else { b }'` works
     * and semicolons separate statements. Arguments after it are $args. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        bind_args(&top, argc, argv, 3);
        int rc = run_program(argv[2], &top);
        scope_free(&top);
        return rc;
    }

    /* script mode: shell.elf path.esh
     * The first non-flag argument is a script to run. This is what makes the
     * system automatable at all -- until now every capability the shell had
     * required a human to type it. */
    if (argc >= 2 && argv[1][0] != '-') {
        bind_args(&top, argc, argv, 2);
        int rc = run_script(argv[1], &top);
        scope_free(&top);
        return rc;
    }

    puts1("EmbLink shell -- structured pipelines. `exit` leaves.\n");
    bool interactive = g_terminal_session || g_console_session;
    if (interactive) open_history();
    if (interactive) source_rc(&top);
    /* No prompt configured: Powerlevel10k's wizard, as zsh starts it */
    if (g_terminal_session && wizard_os_wanted()) (void)wizard_os_run(false, NULL, 0);
    struct edit_host host = { out_write, shell_complete, unfinished, &top, reprompt, shell_highlight };
    g_ed = edit_new(&host);
    /* While the line is edited the shell reads every key itself: no line
     * discipline, no echo, ^C as a byte (the editor's own), CR as CR. */
    struct termios editing = g_prompt_attr;
    editing.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    editing.c_iflag &= ~(tcflag_t)ICRNL;
    editing.c_cc[VMIN] = 1;
    editing.c_cc[VTIME] = 0;
    int status = -1;                          /* the last command's; -1 before the first */
    uint64_t duration = 0;                    /* how long it ran, ms */
    char last_where[200] = "", last_cwd[192] = "";   /* where the last prompt was */
    (void)scope_bind(&top, "status", value_int(0));
    char plain[LINE_MAX_LEN];
    for (;;) {
        const char *line = NULL;
        if (g_terminal_session && g_ed) {
            /* Where the last command left the cursor is not known: zsh's
             * PROMPT_SP. Fill the row and come back -- output that did not end
             * in a newline keeps a reverse '%' after it, and the prompt starts
             * on a row of its own either way. Then the modes a program may have
             * left behind are reset, and the bar cursor of the prompt set. */
            int cols = terminal_cols();
            char fill[2112];
            int k = snprintf(fill, sizeof fill, "\033[?1049l\033[?1l\033>\033[?25h\033[0m\033[5 q\033[1;7m%%\033[0m");
            for (int i = 1; i < cols && k < (int)sizeof fill - 8; i++) fill[k++] = ' ';
            memcpy(fill + k, "\r \r", 3);
            out_write(NULL, fill, (size_t)k + 3);
            if (status >= 0) {
                char done[32];
                snprintf(done, sizeof done, "\033]133;D;%d\007", status);
                puts1(done);
            }
            char where[200], cwd[192], shown[192], mark[256];
            prompt_where(where, sizeof where);
            if (!getcwd(cwd, sizeof cwd)) { cwd[0] = '/'; cwd[1] = 0; }
            memcpy(shown, cwd, sizeof shown);
            for (char *p = shown; *p; p++) if ((unsigned char)*p < 32 || (unsigned char)*p == 127) *p = '?';
            snprintf(mark, sizeof mark, "\033]133;A;edit=shell;cwd=%s\007", shown);
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            /* Powerlevel10k asks about the directory and the repository:
             * fresh listings for every prompt */
            bool p10k = prompt_p10k_source() != NULL;
            if (p10k) prompt_os_fresh();
            shell_highlight_fresh();                   /* programs installed since are commands now */
            const char *user = getenv("USER");
            g_pstate = (struct prompt_state){
                .user = user, .host = getenv("HOSTNAME"), .dir = where, .last_dir = last_where,
                .status = status, .duration_ms = duration,
                .hour = tm ? tm->tm_hour : 0, .minute = tm ? tm->tm_min : 0, .second = tm ? tm->tm_sec : 0,
                .pwd = cwd, .last_pwd = last_cwd, .home = getenv("HOME"),
                .year = tm ? tm->tm_year + 1900 : 1970, .month = tm ? tm->tm_mon + 1 : 1,
                .mday = tm ? tm->tm_mday : 1, .wday = tm ? tm->tm_wday : 4,
                .jobs = p10k ? unfinished_jobs() : 0, .root = user && !strcmp(user, "root"),
                .git = p10k && prompt_os_gitstatus(prompt_p10k_config(), cwd, &g_git) ? &g_git : NULL,
                .fs = p10k ? &prompt_os_fs : NULL };
            prompt_build(&g_pstate, cols, &g_ptext);
            const char *program = getenv("TERM_PROGRAM");
            bool ours = program && !strcmp(program, "EmbLinkTerminal");
            struct edit_prompt prompt = { mark, NULL, "\033[2m...\033[0m ", "\033]133;B\007",
                                          ours ? "\033]133;R\007" : NULL, NULL, NULL };
            prompt_into(&prompt);
            (void)tcsetattr(0, TCSANOW, &editing);
            enum edit_result r = edit_one_line(&prompt, EDIT_VT);
            (void)tcsetattr(0, TCSANOW, &g_prompt_attr);
            puts1("\033[0 q");
            snprintf(last_where, sizeof last_where, "%s", where);
            snprintf(last_cwd, sizeof last_cwd, "%s", cwd);
            if (r == EDIT_EOF) break;
            if (r == EDIT_CANCEL) { status = 130; continue; }
            line = edit_line(g_ed);
        } else if (g_console_session && g_ed) {
            struct edit_prompt prompt = { NULL, "embk> ", "...> ", NULL, NULL, NULL, NULL };
            enum edit_result r = edit_one_line(&prompt, EDIT_DUMB);
            if (r == EDIT_EOF) break;
            if (r == EDIT_CANCEL) continue;
            line = edit_line(g_ed);
        } else {
            puts1("embk> ");
            if (read_plain_line(plain, sizeof plain) < 0) break;
            line = plain;
        }
        if (strcmp(line, "exit") == 0) break;
        if (interactive) hist_push(line);
        if (g_terminal_session) puts1("\033]133;C\007");
        uint64_t started = embk_uptime_ms();
        status = run_line(line, &top);
        duration = embk_uptime_ms() - started;
        (void)scope_bind(&top, "status", value_int(status));
    }
    edit_free(g_ed);
    if (g_console_session) embk_tty_mode(EMBK_TTY_COOKED);
    scope_free(&top);
    return 0;
}
