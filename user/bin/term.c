/* user/bin/term.c -- the EmbLink Terminal: the structured shell, on screen.
 *
 * NOT a teletype. The window is a read-only TRANSCRIPT VIEWER with a single
 * INPUT LINE beneath it: you read above, you type below, and the cursor only
 * ever lives on that one line. Running `ls` prints its output into the viewer
 * rather than into the thing you are typing in.
 *
 * That inverts who owns the line. A classic terminal forwards every keystroke
 * and lets the shell echo -- which only works when input and output share one
 * stream. Here nothing is sent until Enter, so the TERMINAL owns the line
 * buffer, and therefore owns the whole editing surface a Linux user's hands
 * expect: cursor movement (Left/Right/Home/End), kills (Ctrl+U/K/W), history
 * (Up/Down), Ctrl+C to abandon a line, Ctrl+L to clear, Tab completion for
 * paths and commands, and wheel/PgUp scrollback.
 *
 * The transcript is COLOURED: a per-character attribute plane beside the
 * character plane, filled by parsing ANSI SGR (ESC[..m) out of the byte
 * stream. The shell emits SGR only when TERM=emlink says someone can parse it
 * -- the same contract Linux uses -- so pipes and the serial console stay
 * clean bytes.
 *
 * It spawns /shell.elf as a child with both stdio ends piped (Piece-0
 * plumbing: INSTALL_OBJ into the child, fd_install_obj for its own ends).
 * Output is polled with embk_fd_avail() from the runtime's idle hook (a render
 * loop must never block in read()).
 *
 * Closing the window closes the shell FOR FREE: the terminal's exit drops
 * its pipe fds (exit-time reap loop), the shell's read(0) returns EOF, and
 * its REPL exits -- nobody sends signals, the plumbing itself hangs up. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

/* CAPACITY (buffer dimensions) vs the LIVE GRID (what fits right now). The
 * window is resizable, so the grid is measured from the viewport every frame
 * and the buffers are simply big enough for the largest sane window. */
#define SB_COLS  220       /* widest line the scrollback can hold */
#define SB_VROWS 60        /* most transcript rows we can ever render */
static int g_cols = 96;    /* live: chars per line before a hard wrap */
static int g_rows = 22;    /* live: visible transcript lines */
#define SB_ROWS 300        /* scrollback depth (a ring of whole lines) */
#define FD_SHELL_IN  10    /* our WRITE end of the shell's stdin  */
#define FD_SHELL_OUT 11    /* our READ end of the shell's stdout  */

/* --- scrollback: characters + a parallel ATTRIBUTE plane ------------------
 * g_head is the ring slot of the CURRENT (partial) line; g_count how many
 * slots hold real lines; g_view how far the user has paged back (0 = live).
 * Each cell's attribute: low 4 bits = colour (0 default, 1..8 = ANSI 30..37),
 * bit 4 = bright, bit 5 = bold. */
#define AT_COLOR(a)  ((a) & 0x0F)
#define AT_BRIGHT    0x10
#define AT_BOLD      0x20
static char    g_sb [SB_ROWS][SB_COLS + 1];
static uint8_t g_sba[SB_ROWS][SB_COLS];
static int  g_head = 0, g_count = 1, g_col = 0;
static int  g_view = 0;
static int  g_shell = -1;      /* spawn handle */
static bool g_dead = false;
static uint8_t g_attr = 0;     /* current SGR state, applied to incoming chars */

/* --- the command line ------------------------------------------------------
 * The terminal owns the line: nothing reaches the shell until Enter. g_cur is
 * the caret -- edits happen AT it, not only at the end. */
#define IN_MAX 256
#define HIST_MAX 32
static char g_in[IN_MAX];
static int  g_in_n = 0;
static int  g_cur = 0;         /* caret: 0..g_in_n */
static char g_hist[HIST_MAX][IN_MAX];
static int  g_hist_n = 0;      /* how many commands remembered */
static int  g_hist_at = -1;    /* -1 = editing a fresh line, else index back */

static void hist_push_line(const char *s) {
    if (!s[0]) return;
    if (g_hist_n && !strcmp(g_hist[(g_hist_n - 1) % HIST_MAX], s)) return;  /* no dupes */
    snprintf(g_hist[g_hist_n % HIST_MAX], IN_MAX, "%s", s);
    g_hist_n++;
}

/* Walk history: dir -1 = older (Up), +1 = newer (Down). */
static void hist_step(int dir) {
    int avail = g_hist_n < HIST_MAX ? g_hist_n : HIST_MAX;
    if (!avail) return;
    int at = g_hist_at + (dir < 0 ? 1 : -1);
    if (at < 0) {                       /* back to the fresh line */
        g_hist_at = -1; g_in[0] = 0; g_in_n = 0; g_cur = 0; return;
    }
    if (at >= avail) at = avail - 1;
    g_hist_at = at;
    const char *s = g_hist[(g_hist_n - 1 - at) % HIST_MAX];
    snprintf(g_in, sizeof g_in, "%s", s);
    g_in_n = (int)strlen(g_in);
    g_cur = g_in_n;
}

/* --- the prompt ------------------------------------------------------------
 * The shell hands us its working directory as a marker line (0x10 <path> \n)
 * instead of printing a prompt, so the transcript holds only real output and we
 * can draw a proper user@host:cwd$ on the input line -- with the path live, so
 * it follows `cd` the way a shell prompt should. */
#define CWD_MARK 0x10
static char g_cwd[192] = "/";
static int  g_cwd_cap = 0, g_cwd_n = 0;

#define HOSTNAME "emblink"

static const char *prompt_text(void) {
    static char p[288];
    const char *user = getenv("USER");
    const char *home = getenv("HOME");
    if (!user || !*user) user = "user";
    char tilde[192];
    const char *shown = g_cwd;
    size_t hl = home ? strlen(home) : 0;
    /* ~ for home, as every shell does -- the full path is noise in your own
     * directory, which is where you spend most of your time. */
    if (hl && strncmp(g_cwd, home, hl) == 0 && (g_cwd[hl] == '/' || g_cwd[hl] == 0)) {
        snprintf(tilde, sizeof tilde, "~%s", g_cwd + hl);
        shown = tilde;
    }
    snprintf(p, sizeof p, "%s@%s:%s$", user, HOSTNAME, shown);
    return p;
}

/* ring slot of logical line `i` (0 = oldest live line, g_count-1 = current) */
static int sb_slot(int i) {
    return (g_head - (g_count - 1) + i + 2 * SB_ROWS) % SB_ROWS;
}

static void term_newline(void) {
    g_head = (g_head + 1) % SB_ROWS;
    if (g_count < SB_ROWS) g_count++;
    memset(g_sb[g_head], 0, sizeof g_sb[0]);
    memset(g_sba[g_head], 0, sizeof g_sba[0]);
    g_col = 0;
    /* keep a paged-back view anchored on the same CONTENT while new lines
     * arrive underneath -- until the ring saturates and eats it */
    if (g_view > 0 && g_view < g_count - g_rows) g_view++;
    if (g_view > g_count - g_rows) g_view = g_count > g_rows ? g_count - g_rows : 0;
}

/* --- ANSI SGR (ESC [ ... m) ------------------------------------------------
 * Only SGR is honoured -- colour and weight are what a transcript needs. Any
 * other CSI sequence is parsed to its final byte and dropped whole, so cursor
 * games from ported software degrade to plain text instead of garbage. */
static int  g_esc = 0;            /* 0 plain, 1 saw ESC, 2 inside CSI */
static int  g_csi[8], g_csi_n;    /* collected numeric params */
static int  g_csi_cur;            /* param being accumulated (-1 = none yet) */

static void sgr_apply(void) {
    if (g_csi_cur >= 0 && g_csi_n < 8) g_csi[g_csi_n++] = g_csi_cur;
    if (g_csi_n == 0) { g_attr = 0; return; }         /* bare ESC[m = reset */
    for (int i = 0; i < g_csi_n; i++) {
        int p = g_csi[i];
        if      (p == 0)              g_attr = 0;
        else if (p == 1)              g_attr |= AT_BOLD;
        else if (p == 22)             g_attr &= (uint8_t)~AT_BOLD;
        else if (p >= 30 && p <= 37)  g_attr = (uint8_t)((g_attr & ~0x1F) | (p - 30 + 1));
        else if (p >= 90 && p <= 97)  g_attr = (uint8_t)((g_attr & ~0x1F) | (p - 90 + 1) | AT_BRIGHT);
        else if (p == 39)             g_attr &= (uint8_t)~0x1F;
        /* backgrounds / everything else: ignored on purpose */
    }
}

static void term_putc(char c) {
    /* the shell's cwd marker: swallowed whole, never shown */
    if (g_cwd_cap) {
        if (c == '\n') { g_cwd[g_cwd_n] = 0; g_cwd_cap = 0; }
        else if (g_cwd_n < (int)sizeof g_cwd - 1) g_cwd[g_cwd_n++] = c;
        return;
    }
    if (c == CWD_MARK) { g_cwd_cap = 1; g_cwd_n = 0; return; }

    /* escape-sequence states eat their bytes before anything else sees them */
    if (g_esc == 1) {
        if (c == '[') { g_esc = 2; g_csi_n = 0; g_csi_cur = -1; }
        else g_esc = 0;                                  /* lone ESC: dropped */
        return;
    }
    if (g_esc == 2) {
        if (c >= '0' && c <= '9') {
            g_csi_cur = (g_csi_cur < 0 ? 0 : g_csi_cur) * 10 + (c - '0');
        } else if (c == ';') {
            if (g_csi_n < 8) g_csi[g_csi_n++] = g_csi_cur < 0 ? 0 : g_csi_cur;
            g_csi_cur = -1;
        } else if (c >= 0x40 && c <= 0x7E) {             /* final byte */
            if (c == 'm') sgr_apply();
            g_esc = 0;
        }
        return;
    }
    if (c == 0x1B) { g_esc = 1; return; }

    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\t') {                       /* expand to 8-column stops */
        int stop = (g_col / 8 + 1) * 8;
        while (g_col < stop && g_col < g_cols) {
            g_sba[g_head][g_col] = 0;
            g_sb[g_head][g_col++] = ' ';
        }
        g_sb[g_head][g_col] = '\0';
        return;
    }
    if (c == '\f') {                       /* form feed: the shell's `clear` */
        memset(g_sb, 0, sizeof g_sb);
        memset(g_sba, 0, sizeof g_sba);
        g_head = 0;
        g_count = 1;
        g_col = 0;
        g_view = 0;
        return;
    }
    if (c == '\b') {
        if (g_col > 0) { g_col--; g_sb[g_head][g_col] = '\0'; }
        return;
    }
    if ((unsigned char)c < 0x20) return;   /* other control bytes: drop */
    if (g_col >= g_cols) term_newline();   /* hard wrap */
    g_sba[g_head][g_col] = g_attr;
    g_sb[g_head][g_col++] = c;
    g_sb[g_head][g_col] = '\0';
}

static void term_say(const char *s) { while (*s) term_putc(*s++); }

/* --- line-editor edits ----------------------------------------------------- */

static void in_insert(char c) {
    if (g_in_n >= IN_MAX - 1) return;
    memmove(g_in + g_cur + 1, g_in + g_cur, (size_t)(g_in_n - g_cur) + 1);
    g_in[g_cur++] = c;
    g_in_n++;
}

static void in_backspace(void) {
    if (g_cur == 0) return;
    memmove(g_in + g_cur - 1, g_in + g_cur, (size_t)(g_in_n - g_cur) + 1);
    g_cur--; g_in_n--;
}

static void in_delete(void) {                    /* forward delete, at the caret */
    if (g_cur >= g_in_n) return;
    memmove(g_in + g_cur, g_in + g_cur + 1, (size_t)(g_in_n - g_cur));
    g_in_n--;
}

static void in_kill_to_start(void) {             /* Ctrl+U */
    memmove(g_in, g_in + g_cur, (size_t)(g_in_n - g_cur) + 1);
    g_in_n -= g_cur; g_cur = 0;
}

static void in_kill_to_end(void) {               /* Ctrl+K */
    g_in[g_cur] = 0; g_in_n = g_cur;
}

static void in_kill_word(void) {                 /* Ctrl+W */
    int e = g_cur;
    while (g_cur > 0 && g_in[g_cur - 1] == ' ') g_cur--;
    while (g_cur > 0 && g_in[g_cur - 1] != ' ') g_cur--;
    memmove(g_in + g_cur, g_in + e, (size_t)(g_in_n - e) + 1);
    g_in_n -= e - g_cur;
}

/* --- Tab completion --------------------------------------------------------
 * The terminal owns the line, so it owns completion too. It completes the
 * token under the caret against the filesystem (relative to the cwd the shell
 * reports), and -- in command position -- against the shell's builtins and the
 * extern search directories. One match completes; several extend to the common
 * prefix and, when stuck, list into the transcript, exactly as bash does. */
static const char *k_builtins[] = {
    "ls","cat","cd","pwd","mkdir","rmdir","touch","rm","cp","mv","save",
    "ps","kill","env","uptime","date","clear","echo","where","select",
    "sort-by","first","head","last","tail","reverse","get","count","wc",
    "history","which","whoami","hostname","help","exit",
};

struct cand { char name[64]; int is_dir; };
#define CAND_MAX 96

static int cand_add(struct cand *cs, int n, const char *name, int is_dir) {
    if (n >= CAND_MAX) return n;
    snprintf(cs[n].name, sizeof cs[n].name, "%s", name);
    cs[n].is_dir = is_dir;
    return n + 1;
}

static void complete(void) {
    if (g_cur != g_in_n) return;               /* complete at line end only */

    /* the token being completed, and whether it names a command */
    int tok = g_in_n;
    while (tok > 0 && g_in[tok - 1] != ' ') tok--;
    int is_cmd = 1;
    for (int i = 0; i < tok; i++) if (g_in[i] != ' ') { is_cmd = 0; break; }
    const char *token = g_in + tok;

    /* split off any directory part; resolve it against the live cwd */
    const char *slash = strrchr(token, '/');
    char dir[224]; const char *base = token;
    if (slash) {
        size_t dl = (size_t)(slash - token);
        char dpart[160];
        if (dl >= sizeof dpart) return;
        memcpy(dpart, token, dl); dpart[dl] = 0;
        base = slash + 1;
        if (token[0] == '/')      snprintf(dir, sizeof dir, "%s", dl ? dpart : "/");
        else if (token[0] == '~') {
            const char *h = getenv("HOME");
            snprintf(dir, sizeof dir, "%s%s", h ? h : "", dpart + 1);
        }
        else snprintf(dir, sizeof dir, "%s/%s", g_cwd, dpart);
    } else {
        snprintf(dir, sizeof dir, "%s", g_cwd);
    }
    size_t blen = strlen(base);

    static struct cand cs[CAND_MAX];
    int n = 0;

    /* command position: builtins + the extern search dirs, bare names */
    if (is_cmd && !slash) {
        for (size_t i = 0; i < sizeof k_builtins / sizeof k_builtins[0]; i++)
            if (!strncmp(k_builtins[i], base, blen))
                n = cand_add(cs, n, k_builtins[i], 0);
        struct embk_dirent es[64];
        int64_t m = embk_readdir("/system/bin", es, 64);
        for (int64_t i = 0; i < m; i++) {
            size_t l = strlen(es[i].name);
            if (l > 4 && !strcmp(es[i].name + l - 4, ".elf")) {
                char bare[64];
                snprintf(bare, sizeof bare, "%.*s", (int)(l - 4), es[i].name);
                if (!strncmp(bare, base, blen)) n = cand_add(cs, n, bare, 0);
            }
        }
        m = embk_readdir("/data/apps", es, 64);
        for (int64_t i = 0; i < m; i++)
            if (es[i].type == EMBK_DT_DIR && es[i].name[0] != '.' &&
                !strncmp(es[i].name, base, blen))
                n = cand_add(cs, n, es[i].name, 0);
    }

    /* filesystem: everything in `dir` matching the base prefix */
    {
        struct embk_dirent es[64];
        int64_t m = embk_readdir(dir, es, 64);
        for (int64_t i = 0; i < m; i++) {
            if (es[i].name[0] == '.' && blen == 0) { /* hide dotfiles unprompted */ }
            else if (!strncmp(es[i].name, base, blen))
                n = cand_add(cs, n, es[i].name, es[i].type == EMBK_DT_DIR);
        }
    }
    if (n == 0) return;

    /* longest common prefix across every candidate */
    size_t common = strlen(cs[0].name);
    for (int i = 1; i < n; i++) {
        size_t j = 0;
        while (j < common && cs[i].name[j] == cs[0].name[j]) j++;
        common = j;
    }

    if (common > blen) {                       /* progress: extend the token */
        for (size_t j = blen; j < common && g_in_n < IN_MAX - 1; j++)
            in_insert(cs[0].name[j]);
        if (n == 1) in_insert(cs[0].is_dir ? '/' : ' ');
        return;
    }
    if (n == 1) { in_insert(cs[0].is_dir ? '/' : ' '); return; }

    /* stuck with several: show them, the way a second Tab does in bash */
    term_say(prompt_text()); term_putc(' '); term_say(g_in); term_putc('\n');
    for (int i = 0; i < n; i++) {
        if (cs[i].is_dir) { term_say("\x1b[1;34m"); term_say(cs[i].name);
                            term_say("\x1b[0m/"); }
        else term_say(cs[i].name);
        term_say("  ");
    }
    term_putc('\n');
}

/* --- runtime hooks -------------------------------------------------------- */

/* idle: drain the shell's output pipe without blocking; wake the renderer
 * only when something actually arrived. Also notice the shell dying. */
static void term_idle(void) {
    bool got = false;
    for (;;) {
        int64_t n = embk_fd_avail(FD_SHELL_OUT);
        if (n <= 0) break;
        char buf[256];
        int r = (int)embk_read(FD_SHELL_OUT, buf, n < (int64_t)sizeof buf ? (size_t)n : sizeof buf);
        if (r <= 0) break;
        for (int i = 0; i < r; i++) term_putc(buf[i]);
        got = true;
    }
    if (!g_dead && g_shell >= 0 && !embk_proc_alive(g_shell)) {
        g_dead = true;
        term_say("\n[shell exited -- press ESC to close]\n");
        got = true;
    }
    if (got) em_request_frame();
}

/* keys: the full line-editor surface. PgUp/PgDn (and the wheel, in the view)
 * page the scrollback; editing keys work AT the caret; ESC clears the line
 * first and only closes the window when there is nothing to clear. */
static int term_key(int c) {
    if (c == 27) {
        if (g_dead || g_in_n == 0) return 0;     /* nothing to clear -> close */
        g_in[0] = 0; g_in_n = 0; g_cur = 0; g_hist_at = -1;
        em_request_frame();
        return 1;
    }
    if (c == EMBK_KEY_PGUP || c == EMBK_KEY_PGDN) {
        int max_back = g_count > g_rows ? g_count - g_rows : 0;
        if (c == EMBK_KEY_PGUP) g_view += g_rows / 2;
        else                    g_view -= g_rows / 2;
        if (g_view > max_back) g_view = max_back;
        if (g_view < 0) g_view = 0;
        em_request_frame();
        return 1;
    }
    if (g_dead) return 1;

    switch (c) {
    case EMBK_KEY_UP:    hist_step(-1); break;
    case EMBK_KEY_DOWN:  hist_step(+1); break;
    case EMBK_KEY_LEFT:  if (g_cur > 0) g_cur--; break;
    case EMBK_KEY_RIGHT: if (g_cur < g_in_n) g_cur++; break;
    case EMBK_KEY_HOME:  case 0x01: g_cur = 0;      break;   /* Home / Ctrl+A */
    case EMBK_KEY_END:              g_cur = g_in_n; break;   /* End  / Ctrl+E */
    case 0x03:                                   /* Ctrl+C: abandon the line */
        term_say(prompt_text()); term_putc(' ');
        term_say(g_in); term_say("^C\n");
        g_in[0] = 0; g_in_n = 0; g_cur = 0; g_hist_at = -1;
        break;
    case 0x0C:                                   /* Ctrl+L: clear the screen */
        term_putc('\f');
        break;
    case 0x15: in_kill_to_start(); break;        /* Ctrl+U */
    case 0x0B: in_kill_to_end();   break;        /* Ctrl+K */
    case 0x17: in_kill_word();     break;        /* Ctrl+W */
    case '\t': complete();         break;
    case '\b': in_backspace();     break;
    case EMBK_KEY_DEL: in_delete(); break;
    case '\n': case '\r': {
        if (g_view != 0) g_view = 0;
        /* Send the whole line at once. The shell echoes it after our written
         * prompt, completing the transcript line into "user@host:~$ ls" ahead
         * of the output -- the record of what actually ran. */
        hist_push_line(g_in);
        g_hist_at = -1;
        term_say(prompt_text());
        term_putc(' ');
        char nl = '\n';
        if (g_in_n) write(FD_SHELL_IN, g_in, (size_t)g_in_n);
        write(FD_SHELL_IN, &nl, 1);
        g_in[0] = 0; g_in_n = 0; g_cur = 0;
        break;
    }
    default:
        if (c >= 0x20 && c <= 0x7E) {
            if (g_view != 0) g_view = 0;         /* typing = snap back to live */
            in_insert((char)c);
        }
        break;
    }
    em_request_frame();
    return 1;                                    /* never reaches the toolkit */
}

/* --- spawn the shell with both stdio ends piped --------------------------- */
static bool term_spawn_shell(void) {
    int pin[2], pout[2];                         /* {read_h, write_h} */
    if (embk_pipe(pin) != 0) return false;       /* shell stdin */
    if (embk_pipe(pout) != 0) {
        embk_close_handle(pin[0]); embk_close_handle(pin[1]);
        return false;
    }

    struct embk_spawn_file_action acts[3];
    memset(acts, 0, sizeof acts);
    acts[0].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;   /* stdin  <- pin read end */
    acts[0].target_fd = 0;
    acts[0].src_obj_handle = pin[0];
    acts[1].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;   /* stdout -> pout write end */
    acts[1].target_fd = 1;
    acts[1].src_obj_handle = pout[1];
    acts[2].kind = EMBK_SPAWN_ACTION_INSTALL_OBJ;   /* stderr -> same pipe */
    acts[2].target_fd = 2;
    acts[2].src_obj_handle = pout[1];

    char *argv[] = { "/system/bin/shell.elf", NULL };
    /* Start the shell in the user's HOME (docs/USERSPACE.md §5): cwd is never
     * inherited, so the terminal -- the session spawner here -- NAMES it via PWD,
     * and the shell's crt0 seeds cwd from it. TERM=emlink is the colour
     * contract: it tells the shell someone on the other end parses SGR. */
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/";
    char home_env[160], pwd_env[160];
    snprintf(home_env, sizeof home_env, "HOME=%s", home);
    snprintf(pwd_env, sizeof pwd_env, "PWD=%s", home);
    char user_env[64];
    snprintf(user_env, sizeof user_env, "USER=%s",
             getenv("USER") ? getenv("USER") : "user");
    char *senv[] = { home_env, pwd_env, user_env, "TERM=emlink", NULL };
    int64_t h = embk_spawn_env("/system/bin/shell.elf", argv, senv, acts, 3);

    /* our copies of the CHILD's ends must go, whatever happened -- EOF
     * accounting depends on it */
    embk_close_handle(pin[0]);
    embk_close_handle(pout[1]);
    if (h < 0) {
        embk_close_handle(pin[1]); embk_close_handle(pout[0]);
        return false;
    }
    g_shell = (int)h;

    /* our ends become plain fds; the handles are then redundant */
    embk_fd_install_obj(pin[1],  FD_SHELL_IN);
    embk_fd_install_obj(pout[0], FD_SHELL_OUT);
    embk_close_handle(pin[1]);
    embk_close_handle(pout[0]);
    return true;
}

/* --- the view -------------------------------------------------------------- */

/* the ANSI palette, tuned for the dark ground (normal / bright) */
static Color ansi_color(uint8_t attr, Color dflt) {
    static const Color norm[8] = {
        { .r=.35f, .g=.37f, .b=.40f, .a=1 },   /* black (visible grey) */
        { .r=.87f, .g=.42f, .b=.42f, .a=1 },   /* red */
        { .r=.45f, .g=.78f, .b=.51f, .a=1 },   /* green */
        { .r=.86f, .g=.75f, .b=.44f, .a=1 },   /* yellow */
        { .r=.47f, .g=.65f, .b=.92f, .a=1 },   /* blue */
        { .r=.78f, .g=.56f, .b=.86f, .a=1 },   /* magenta */
        { .r=.44f, .g=.79f, .b=.80f, .a=1 },   /* cyan */
        { .r=.88f, .g=.89f, .b=.91f, .a=1 },   /* white */
    };
    static const Color bright[8] = {
        { .r=.48f, .g=.50f, .b=.54f, .a=1 },
        { .r=.96f, .g=.54f, .b=.54f, .a=1 },
        { .r=.56f, .g=.90f, .b=.62f, .a=1 },
        { .r=.95f, .g=.86f, .b=.55f, .a=1 },
        { .r=.58f, .g=.75f, .b=.98f, .a=1 },
        { .r=.88f, .g=.67f, .b=.95f, .a=1 },
        { .r=.55f, .g=.90f, .b=.91f, .a=1 },
        { .r=.97f, .g=.97f, .b=.99f, .a=1 },
    };
    int ci = AT_COLOR(attr);
    if (ci == 0) return dflt;
    return (attr & AT_BRIGHT) ? bright[ci - 1] : norm[ci - 1];
}

/* Per-run scratch: substrings handed to Text() must outlive the frame build,
 * so every run gets its own slot rather than sharing one static buffer. */
#define MAX_RUNS 10
static char s_run[SB_VROWS][MAX_RUNS][SB_COLS + 1];

/* one transcript row, split into runs of equal attribute */
static void row_runs(int row_i, const char *s, const uint8_t *a, Color dflt) {
    HStack(.spacing = 0, .align = Center) {
        int i = 0, run = 0;
        while (s[i] && run < MAX_RUNS) {
            uint8_t at = a[i];
            int j = i;
            while (s[j] && a[j] == at) j++;
            int len = j - i; if (len > SB_COLS) len = SB_COLS;
            memcpy(s_run[row_i][run], s + i, (size_t)len);
            s_run[row_i][run][len] = 0;
            EmV t = Text(s_run[row_i][run]).caption().color(ansi_color(at, dflt));
            if (at & AT_BOLD) t.bold();
            i = j; run++;
        }
    }
}

static void term_view(void) {
    /* ONE ground colour for the whole window -- chrome, transcript, command
     * line and any slack. A neutral near-black with a slight cool cast: it
     * stays out of the way of the text, which is the only thing here worth
     * looking at, and the prompt is the single spot of colour. */
    const Color TERM_BG    = { .r=.086f, .g=.090f, .b=.106f, .a=1.f };  /* the ground */
    const Color TERM_INBG  = { .r=.125f, .g=.133f, .b=.156f, .a=1.f };  /* input, a hair lifted */
    const Color TERM_TEXT  = { .r=.855f, .g=.871f, .b=.898f, .a=1.f };  /* soft off-white */
    const Color TERM_PROMPT= { .r=.435f, .g=.780f, .b=.612f, .a=1.f };  /* calm green */

    Window("Terminal", .background = TERM_BG) {
        WindowBar("Terminal", .background = TERM_BG) {
            /* A plain click. CloseGrip is a PULL handle -- it wants a deliberate
             * drag before it fires, which is a fine guard for something
             * destructive and pure friction for closing a window. */
            MinimizeButton();                       /* park it; the dock brings it back */
            if (CloseButton().clicked()) exit(0);   /* window is reaped, shell EOFs */
        }
        /* the VIEWER: a read-only transcript of everything the shell printed */
        VStack(.spacing = 1, .px = 12, .pt = 10, .pb = 4, .align = Leading,
               .grow = 1, .background = TERM_BG) {
            /* the wheel pages the scrollback -- up rolls back in time */
            float wd = ui_take_wheel();
            if (wd != 0.0f) {
                int max_back = g_count > g_rows ? g_count - g_rows : 0;
                g_view += (int)(wd * 3.0f);
                if (g_view > max_back) g_view = max_back;
                if (g_view < 0) g_view = 0;
                em_request_frame();
            }
            int start = g_count - g_rows - g_view;
            if (start < 0) start = 0;
            for (int r = 0; r < g_rows; r++) {
                /* NO `continue`/`break` inside a container's brace scope: the
                 * EmUI containers are for-loop MACROS, so a bare continue
                 * targets the MACRO's hidden loop and silently skips the rest
                 * of the body -- it rendered an entirely blank terminal until
                 * this became an if/else. */
                if (r == 0 && g_view > 0) {
                    /* paged back: the top row becomes the position marker */
                    static char mark[64];
                    snprintf(mark, sizeof mark,
                             "-- scrollback: %d line(s) back (wheel/PgDn -> live) --", g_view);
                    Text(mark).caption().secondary();
                } else {
                    int logical = start + r;
                    if (logical >= 0 && logical < g_count && g_sb[sb_slot(logical)][0]) {
                        int slot = sb_slot(logical);
                        row_runs(r, g_sb[slot], g_sba[slot], TERM_TEXT);
                    } else {
                        Text(" ").caption().color(TERM_TEXT);  /* empty collapses (V7) */
                    }
                }
            }
        }

        /* A grow-only gap: the transcript is a fixed number of rows, so `.grow`
         * on it cannot absorb the leftover height -- this does, pressing the
         * command line flush to the bottom edge instead of leaving it floating
         * with dead space beneath. */
        Spacer();

        /* the COMMAND LINE: the only place a cursor lives. One row, pinned
         * under the transcript -- the caret is drawn AT the edit position, so
         * Left/Right/Home/End visibly move it through the text. */
        HStack(.height = 28, .align = Center, .spacing = 8, .px = 12, .pb = 2,
               .background = TERM_INBG) {
            /* Grid calibration, free of charge: the prompt is drawn in the
             * same mono face as the transcript and we KNOW its length, so its
             * measured box gives the character advance and the line height --
             * no font API, no hidden probe node. Reads LAST frame's geometry
             * (one frame of lag is invisible while dragging a grip). */
            HStack(.align = Center) {
                float bx, by, bw, bh;
                if (ui_open_rect(&bx, &by, &bw, &bh) && bw > 1 && bh > 1) {
                    int plen = (int)strlen(prompt_text());
                    if (plen > 0) {
                        float cw = bw / (float)plen, lh = bh + 1.0f;
                        int cols = (int)((em_viewport_width() - 28.0f) / cw);
                        int rows = (int)((em_viewport_height() - 46.0f - 34.0f) / lh);
                        g_cols = cols < 20 ? 20 : cols > SB_COLS ? SB_COLS : cols;
                        g_rows = rows < 3  ? 3  : rows > SB_VROWS ? SB_VROWS : rows;
                    }
                }
                Text(prompt_text()).caption().bold().color(TERM_PROMPT);
            }
            if (g_dead) {
                Text("(shell exited)").caption().color(TERM_TEXT);
            } else {
                HStack(.spacing = 0, .align = Center) {
                    static char pre[IN_MAX], post[IN_MAX];
                    snprintf(pre,  sizeof pre,  "%.*s", g_cur, g_in);
                    snprintf(post, sizeof post, "%s",   g_in + g_cur);
                    if (pre[0])  Text(pre).caption().color(TERM_TEXT);
                    Text("|").caption().color(TERM_PROMPT);
                    if (post[0]) Text(post).caption().color(TERM_TEXT);
                }
            }
        }
    }
}

int main(void) {
    if (!term_spawn_shell()) {
        term_say("can't start /shell.elf\n");
        g_dead = true;
    }
    em_set_key_hook(term_key);
    em_set_idle_hook(term_idle);

    static EmApp app = {
        .title  = "Terminal",
        .size   = { 740, 480 },
        .theme  = Dark,
        .chrome = Chromeless,
        .resize = Resizable,
        .view   = term_view,
        .font   = "/system/fonts/mono.ttf",   /* DejaVu Sans Mono -- aligned table columns */
    };
    int rc = em_app_run(&app);
    /* window closed: our pipe fds die with us (reap loop) -> the shell's
     * read(0) EOFs -> its REPL exits -> reap. Collect it if still around. */
    if (g_shell >= 0) embk_wait(g_shell);
    return rc;
}
