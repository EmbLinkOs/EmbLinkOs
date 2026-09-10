/* ==========================================================================
 * main.c -- the EmbLink shell driver (shell.elf). Ties the stages together:
 * read a line -> lex -> parse -> evaluate -> sink.
 *
 * Two modes:
 *   shell.elf              interactive REPL on fds 0/1 (the console)
 *   shell.elf -c "line"    run ONE line, exit 0 on success / 1 on error
 *                          (what `test shell` and scripts drive)
 *
 * The SINK (the sketch's last box): if fd 3 is a structured pipe
 * (sval_structured_out -- the shell itself was composed into a bigger
 * pipeline), EMIT the result as a frame; otherwise pretty-print to fd 1.
 *
 * Line editing is deliberately minimal: the kernel console delivers raw
 * keystrokes with NO echo, so the shell echoes what it reads and handles
 * backspace itself. History/cursor keys are the future terminal app's job.
 * ========================================================================== */
#include "lex/lex.h"
#include "parse/parse.h"
#include "eval/eval.h"
#include "eval/exec.h"
#include "sval/sval.h"
#include "hist/hist.h"
#include "embk.h"        /* EMBK_KEY_UP/DOWN -- the kernel's private arrow codes */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>


#define LINE_MAX_LEN 512

/* True when THIS shell's stdin is the machine console and we've put it in raw mode
 * -- i.e. we own the console session. False under the Terminal app or '-c'
 * (stdin is a pipe, not the console): then the global console mode is someone
 * else's property and we must never touch it. same fstat-shape probe as sval.c's
 * fd_is_fifo, for the same reason: ride the standard stat shape the newlib stub 
 *already fills. */
static int g_console_session = 0;

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
}

void shell_tty_resume(void) {
    if (g_console_session) embk_tty_mode(EMBK_TTY_RAW);
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
 * where the kernel delivers it as the byte 0x03 and read_line treats it as a
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

    /* Claim ^C for the duration. Only when we own the console session: under
     * the Terminal app or `-c`, the route belongs to whoever spawned us and
     * taking it would interrupt the wrong thing. */
    if (g_console_session) {
        (void)embk_intr_take();                 /* drop anything stale */
        (void)embk_intr_catch(1);
        (void)embk_console_interrupt_route(0);  /* 0 = self */
    }

    struct exec_out out = { FLOW_NORMAL, value_null(), false };
    int rc = block_exec(prog, top, print_sink, NULL, &out);

    if (g_console_session) {
        (void)embk_console_interrupt_route(-1); /* ^C is a byte again */
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
    return rc != 0 ? 1 : 0;
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

/* Wipe the on-screen line and replace it with `text` -- what an Up/Down
 * recall does. "\b \b" per char is the portable erase (back up, overwrite
 * with a space, back up again); the terminal and the kernel console both
 * understand it, so no cursor-addressing escapes are needed. */
static void line_replace(char *buf, size_t cap, size_t *len, const char *text) {
    while (*len > 0) { puts1("\b \b"); (*len)--; }
    if (!text) { buf[0] = '\0'; return; }
    size_t n = strlen(text);
    if (n >= cap) n = cap - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';
    *len = n;
    if (n) write(1, buf, n);                    /* echo the recalled line */
}

/* -------------------------------------------------------------------------
 * Interactive line read: echo (the console doesn't), backspace, enter, and
 * Up/Down history recall.
 *
 * The kernel delivers arrows as its own private single-byte codes
 * (EMBK_KEY_UP/DOWN = 0x13/0x14, see the keyboard driver) -- NOT ANSI escape
 * sequences -- so recall is a plain byte check, no escape-state machine.
 * That's true whether the shell is read by the Terminal (which forwards the
 * codes) or the kernel console (whose fd-0 read yields them directly).
 *
 * `browse` = how many steps back from the live line: 0 = the line being
 * typed, 1 = newest history entry, ... count = oldest. Down past the newest
 * returns to an EMPTY line (bash returns to the partially-typed line; we
 * don't stash it -- a deliberate simplification, documented).
 * Returns the line length, or -1 on read error / EOF.
 * ------------------------------------------------------------------------- */
static int read_line(char *buf, size_t cap) {
    size_t len = 0;
    size_t browse = 0;
    for (;;) {
        char c;
        int n = read(0, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n' || c == '\r') {
            puts1("\n");
            buf[len] = '\0';
            hist_push(buf);                     /* empty/dup lines self-filter */
            return (int)len;
        }
        if (c == '\b' || c == 0x7f) {          /* backspace / DEL */
            if (len > 0) {
                len--;
                puts1("\b \b");                 /* erase on screen */
            }
            continue;
        }
        if (c == (char)EMBK_KEY_UP) {
            if (browse < hist_count()) {
                browse++;
                line_replace(buf, cap, &len, hist_get(hist_count() - browse));
            }
            continue;
        }
        if (c == (char)EMBK_KEY_DOWN) {
            if (browse > 0) {
                browse--;
                line_replace(buf, cap, &len,
                             browse ? hist_get(hist_count() - browse) : NULL);
            }
            continue;
        }
        if (c == 0x04) {                            /* ^D = EOF */
            if (len == 0) return -1;             /* empty line = leave */
            continue;                            /* non-empty line = ignore */
        }
        if (c == 0x03) {                            /* ^C = abandon this line, re-prompt.
                                                     * The line is NOT hist_push'd (that is
                                                     * the Enter path only), so a killed
                                                     * line never enters history -- Up then
                                                     * recalls the previous REAL command. */
            puts1("^C\n");
            buf[0] = '\0';
            return 0;                            /* empty line: main loop re-prompts */
        }
        if ((unsigned char)c < 0x20) continue;  /* ignore other control chars */
        if (len + 1 < cap) {
            buf[len++] = c;
            write(1, &c, 1);                    /* echo */
            browse = 0;                          /* typing leaves history */
        }
    }
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


    if (stdin_is_console()) {
        embk_tty_mode(EMBK_TTY_RAW);
        /* At OUR prompt, ^C is a LINE-EDIT key (kill the input line), NOT a
         * cancellation -- so route it to NOBODY, which makes it arrive as the
         * byte 0x03 that read_line handles. Routing ^C to SELF was wrong: it
         * cancels this process, and the cancel flag is STICKY with no way to
         * clear it, so the shell could never read again -- one ^C exited it.
         * eval_extern hands the route to a CHILD for its lifetime (there ^C IS a
         * real cancellation) and reclaims it -- back to this NOBODY state -- after. */
        (void)embk_console_interrupt_route(-1);
        g_console_session = 1;
    }

    /* Colour is opt-IN by the display: the GUI terminal names itself via
     * TERM=emlink and parses SGR; the serial console and plain pipes never
     * see an escape byte. The same contract Linux uses (TERM decides). */
    {
        const char *t = getenv("TERM");
        if (!g_console_session && t && strncmp(t, "emlink", 6) == 0)
            sval_set_colour(1);
    }

    /* The executor needs the session's top scope (user commands resolve
     * against it) and the sink their bodies print through, before ANY program
     * runs -- including the one-shot forms below. */
    exec_init(&top, print_sink, NULL);
    exec_set_interrupt_check(shell_interrupted);

    /* one-shot mode: shell.elf -c "ls | where size > 1mb"
     * The argument is a whole PROGRAM, so `-c 'if x { a } else { b }'` works
     * and semicolons separate statements. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        int rc = run_program(argv[2], &top);
        scope_free(&top);
        return rc;
    }

    /* script mode: shell.elf path.esh
     * The first non-flag argument is a script to run. This is what makes the
     * system automatable at all -- until now every capability the shell had
     * required a human to type it. */
    if (argc >= 2 && argv[1][0] != '-') {
        int rc = run_script(argv[1], &top);
        scope_free(&top);
        return rc;
    }

    puts1("EmbLink shell -- structured pipelines. `exit` leaves.\n");
    char line[LINE_MAX_LEN];
    for (;;) {
        /* The GUI terminal draws its OWN prompt (user@host:cwd$) on its input
         * line, so printing "embk> " into its transcript would just be a second
         * prompt sitting in the scrollback. Instead we hand it the one thing it
         * cannot know -- our current directory -- as a marker line it consumes
         * and never shows. The serial console still gets a human prompt. */
        if (g_console_session) {
            puts1("embk> ");
        } else {
            char cwd[192];
            if (!getcwd(cwd, sizeof cwd)) { cwd[0] = '/'; cwd[1] = 0; }
            puts1("\x10"); puts1(cwd); puts1("\n");
        }
        int n = read_line(line, sizeof line);
        if (n < 0) break;                      /* console EOF/error: leave */
        if (strcmp(line, "exit") == 0) break;
        (void)run_line(line, &top);
    }
    if (g_console_session) embk_tty_mode(EMBK_TTY_COOKED);
    scope_free(&top);
    return 0;
}
