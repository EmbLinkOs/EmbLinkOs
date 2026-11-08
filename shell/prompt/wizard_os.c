/* ==========================================================================
 * wizard_os.c -- `prompt configure` on the terminal. See wizard_os.h.
 *
 * What p10k configure checks before it starts (_p9k_can_configure), the
 * wizard on fds 0 and 1 in raw mode -- keys one at a time, Ctrl+C a key --
 * and then the files: the old configuration kept aside when the person said
 * to overwrite it, the new one, and prompt.esh, which loads it in every shell
 * from now on (the wizard loads it in this one).
 * ========================================================================== */
#include "prompt/wizard_os.h"
#include "prompt/wizard.h"
#include "prompt/prompt.h"
#include "embk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define STYLES "/system/share/powerlevel10k"

/* --- the terminal -------------------------------------------------------------- */

struct term { unsigned char buf[64]; int n, at, cols, rows; };

static void term_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) { *cols = ws.ws_col; *rows = ws.ws_row; }
    else { *cols = 80; *rows = 24; }
}

static void size(void *ctx, int *cols, int *rows) { (void)ctx; term_size(cols, rows); }

static void out(void *ctx, const char *bytes, size_t n) {
    (void)ctx;
    while (n) {
        ssize_t w = write(1, bytes, n);
        if (w <= 0) return;
        bytes += w;
        n -= (size_t)w;
    }
}

/* The next byte within `ms`, or the window's new size first. */
static int byte(struct term *t, int ms) {
    if (t->at < t->n) return t->buf[t->at++];
    for (int waited = 0;; waited += 10) {
        int c, r;
        term_size(&c, &r);
        if (c != t->cols || r != t->rows) { t->cols = c; t->rows = r; return WIZARD_RESIZED; }
        if (embk_fd_poll(0, EMBK_POLLIN) & EMBK_POLLIN) {
            ssize_t n = read(0, t->buf, sizeof t->buf);
            if (n <= 0) return WIZARD_EOF;
            t->n = (int)n;
            t->at = 1;
            return t->buf[0];
        }
        if (waited >= ms) return WIZARD_TIMEOUT;
        embk_sleep_ms(10);
    }
}

/* A key: a byte typed, the arrows' and function keys' sequences left out
 * (p10k reads them as letters and digits, and an F5 could answer "1"). */
static int key(void *ctx, int ms) {
    struct term *t = ctx;
    int c = byte(t, ms);
    if (c != 0x1b) return c;
    int d = byte(t, 30);
    if (d == '[') {
        int e;
        do e = byte(t, 30); while (e >= 0 && !(e >= 0x40 && e <= 0x7e));
    } else if (d == 'O') (void)byte(t, 30);
    else if (d == ']') {
        int e, last = 0;
        while ((e = byte(t, 30)) >= 0 && e != 7 && !(last == 0x1b && e == '\\')) last = e;
    }
    return d == WIZARD_RESIZED || d == WIZARD_EOF ? d : WIZARD_TIMEOUT;
}

/* --- files ------------------------------------------------------------------------- */

/* Whether directory `dir` lists `name`: a missing file looked up costs a line
 * in the kernel's log. */
static bool listed(const char *dir, const char *name) {
    static struct embk_dirent ents[128];
    int64_t n = embk_readdir(dir, ents, 128);
    for (int64_t i = 0; i < n; i++) if (!strcmp(ents[i].name, name)) return true;
    return false;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *s = malloc(cap);
    for (size_t got; s && (got = fread(s + n, 1, cap - n - 1, f)) > 0;) {
        n += got;
        if (n + 1 == cap) {
            char *t = realloc(s, cap *= 2);
            if (!t) { free(s); s = NULL; }
            else s = t;
        }
    }
    fclose(f);
    if (s) s[n] = 0;
    return s;
}

static bool spill(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* --- the wizard ------------------------------------------------------------------- */

static void say(char *err, size_t cap, const char *why) { if (err && cap) snprintf(err, cap, "prompt configure: %s", why); }

bool wizard_os_wanted(void) {
    if (prompt_p10k_source()) return false;
    for (size_t i = 0; i < prompt_opt_count(); i++)
        if (strcmp(prompt_opt_key(i), "mode")) return false;           /* p10k too leaves MODE out */
    const char *no = getenv("EMBLINK_NO_WIZARD");
    if (no && no[0] && strcmp(no, "0")) return false;
    int s = embk_session(EMBK_SESSION_STATUS, 0);
    return !(s >= 0 && (s & EMBK_SESSION_LIVE));
}

/* The header's "Wizard options:" lines, from the configuration. */
static void options_lines(const char *config, char *out, size_t cap) {
    out[0] = 0;
    const char *p = strstr(config, "# Wizard options: ");
    if (!p) return;
    const char *end = strstr(p, "\n# Type ");
    size_t n = end ? (size_t)(end - p) + 1 : strlen(p);
    snprintf(out, cap, "%.*s", (int)n, p);
}

int wizard_os_run(bool force, char *err, size_t cap) {
    /* _p9k_can_configure */
    const char *home = getenv("HOME");
    if (!home || !home[0] || !strcmp(home, "/")) { say(err, cap, "there is no home to keep the configuration in"); return -1; }
    char config_dir[224], dir[240], config[272], esh[272], backup[300], styles[200];
    snprintf(config_dir, sizeof config_dir, "%s/.config", home);
    snprintf(dir, sizeof dir, "%s/shell", config_dir);
    (void)embk_mkdir(config_dir);                       /* there already: fine */
    (void)embk_mkdir(dir);
    if (!listed(config_dir, "shell")) { say(err, cap, "cannot create ~/.config/shell"); return -1; }
    const char *sd = getenv("PROMPT_STYLES");
    snprintf(styles, sizeof styles, "%s", sd && sd[0] ? sd : STYLES);
    static const char *const style_files[] = { "p10k-lean.zsh", "p10k-lean-8colors.zsh", "p10k-classic.zsh",
                                               "p10k-rainbow.zsh", "p10k-pure.zsh" };
    for (size_t i = 0; i < sizeof style_files / sizeof *style_files; i++)
        if (!listed(styles, style_files[i])) {
            char why[256];
            snprintf(why, sizeof why, "%s/%s is not there", styles, style_files[i]);
            say(err, cap, why);
            return -1;
        }
    const char *term_name = getenv("TERM");
    struct termios saved;
    if (!term_name || !term_name[0] || !strcmp(term_name, "dumb") || tcgetattr(0, &saved) != 0) {
        say(err, cap, "it needs a terminal to draw its pages on");
        return -1;
    }
    struct term t = { { 0 }, 0, 0, 0, 0 };
    term_size(&t.cols, &t.rows);
    if (t.cols < 47 || t.rows < 14) {
        say(err, cap, "the terminal is too small; it must be at least 47 columns by 14 lines");
        return -1;
    }

    snprintf(config, sizeof config, "%s/p10k.zsh", dir);
    snprintf(esh, sizeof esh, "%s/prompt.esh", dir);
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char stamp[32] = "old", date[64] = "";
    if (tm) {
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
        strftime(date, sizeof date, "%Y-%m-%d at %H:%M %Z", tm);
    }
    snprintf(backup, sizeof backup, "%s/p10k.zsh.%s", dir, stamp);
    char backup_u[64];
    snprintf(backup_u, sizeof backup_u, "~/.config/shell/p10k.zsh.%s", stamp);
    const char *colorterm = getenv("COLORTERM");
    struct wizard_env env = {
        force,
        strstr(term_name, "256color") != NULL || strstr(term_name, "-direct") != NULL,
        colorterm && (!strcmp(colorterm, "truecolor") || !strcmp(colorterm, "24bit")),
        listed(dir, "p10k.zsh"),
        "~/.config/shell/p10k.zsh", backup_u, "~/.config/shell/rc.esh" };

    /* raw: each key as it is typed, unseen, Ctrl+C a key like any other */
    struct termios raw = saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(tcflag_t)ICRNL;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    (void)tcsetattr(0, TCSANOW, &raw);
    struct wizard_io io = { &t, out, key, size };
    struct wizard_answers *a = malloc(sizeof *a);
    enum wizard_end end = a ? wizard_run(&io, &env, a) : WIZARD_QUIT;
    (void)tcsetattr(0, TCSANOW, &saved);
    if (end != WIZARD_DONE) { free(a); return 0; }

    char tmpl[24], path[240];
    wizard_template(a, tmpl, sizeof tmpl);
    snprintf(path, sizeof path, "%s/p10k-%s.zsh", styles, tmpl);
    char *base = slurp(path), *text = base ? wizard_config(a, base, date) : NULL;
    bool keep = a->overwrite;
    free(a);
    free(base);
    if (!text) { say(err, cap, "cannot read the style's template"); return -1; }
    if (keep) {
        char *old = slurp(config);
        if (!old || !spill(backup, old)) {
            free(old);
            free(text);
            say(err, cap, "cannot keep the old configuration aside; it is as it was");
            return -1;
        }
        free(old);
    }
    char opts[1024], *loader = malloc(strlen(date) + 2048);
    options_lines(text, opts, sizeof opts);
    if (loader)
        snprintf(loader, strlen(date) + 2048,
                 "# ~/.config/shell/prompt.esh -- written by `prompt configure` on %s.\n"
                 "# Type `prompt configure` to answer again. The prompt is Powerlevel10k's,\n"
                 "# configured in p10k.zsh beside this file as p10k configure writes it.\n"
                 "# The shell reads this before rc.esh: settings of your own go there.\n"
                 "#\n%s"
                 "let p10k = $(prompt import-p10k \"~/.config/shell/p10k.zsh\")\n",
                 date, opts);
    bool written = spill(config, text) && loader && spill(esh, loader);
    free(loader);
    struct p10k_import rep;
    bool loaded = written && prompt_p10k_load(config, text, &rep);
    free(text);
    if (!written) { say(err, cap, "cannot write ~/.config/shell"); return -1; }
    if (!loaded) {
        char why[240];
        snprintf(why, sizeof why, "the configuration does not read: %s", rep.error);
        say(err, cap, why);
        return -1;
    }
    return 1;
}
