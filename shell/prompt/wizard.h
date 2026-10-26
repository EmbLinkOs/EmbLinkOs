/* ==========================================================================
 * wizard.h -- `prompt configure`: Powerlevel10k's configuration wizard.
 *
 * The questions p10k configure asks, in its order, on its pages: whether the
 * font draws a diamond, a lock, an arrow (which says what POWERLEVEL9K_MODE
 * the font can do), then the style, the character set, colours, the time,
 * separators, heads, tails, the height, the connection, the frame, its
 * colour, spacing, icons, flow and the transient prompt -- each page showing
 * the prompts to choose between. A page that does not fit the window drops
 * its blank lines, then its diagrams, then its prompts, as p10k's does.
 * Instant prompt is zsh's (it draws the prompt before .zshrc has run), so it
 * is not asked; nor is p10k's last question, about changing .zshrc.
 *
 * The answers make the configuration p10k would: its style's template
 * (/system/share/powerlevel10k) with the same lines changed, under the same
 * "Wizard options:" header (wizard_config). tools/golden_wizard.py holds
 * both, page for page and line for line, to recordings of p10k configure
 * itself (tests/prompt/wizard, tools/record_wizard.py).
 *
 * PURE: the terminal is wizard_io -- what to write, the next key, the size --
 * so the host drives it with recorded keys (tests/prompt/wizard_dump.c).
 * ========================================================================== */
#ifndef __EMBK_WIZARD_H__
#define __EMBK_WIZARD_H__

#include <stdbool.h>
#include <stddef.h>

#define WIZARD_TIMEOUT (-1)    /* nothing typed in the time given */
#define WIZARD_RESIZED (-2)    /* the window's size changed */
#define WIZARD_EOF     (-3)    /* the input is gone: as Ctrl+C */

struct wizard_io {
    void *ctx;
    void (*write)(void *ctx, const char *bytes, size_t n);
    /* The next byte typed within `ms` milliseconds (escape sequences left
     * out), or one of the three above. */
    int (*key)(void *ctx, int ms);
    void (*size)(void *ctx, int *cols, int *rows);
};

/* What the wizard knows of where it runs. */
struct wizard_env {
    bool force;                /* `prompt configure`; false: a shell found no prompt configuration */
    bool colors256;            /* the terminal has 256 colours (TERM) */
    bool truecolor;            /* and 24-bit ones (COLORTERM) */
    bool exists;               /* the configuration file is there: ask to overwrite it */
    const char *file;          /* where it goes, as shown: ~/.config/shell/p10k.zsh */
    const char *backup;        /* where the old one goes, as shown */
    const char *rc;            /* where a line to keep the wizard away goes, as shown */
};

/* The answers: what the configuration is made from. */
struct wizard_answers {
    char mode[32];             /* POWERLEVEL9K_MODE */
    char icon_padding[16];
    char style[16];            /* lean, lean_8colors, classic, rainbow, pure */
    int color;                 /* 1-4: of the frame (or the background, classic) */
    int frame_color[4], bg_color[4], sep_color[4], prefix_color[4];
    int num_lines, empty_line, left_frame, right_frame, transient, cap_python;
    char gap_char[8], time[16];
    char left_sep[16], right_sep[16], left_subsep[16], right_subsep[16];
    char left_head[16], right_head[16], left_tail[16], right_tail[16];
    char extra_icons[5][32], prefixes[3][8];
    char pure_color[7][12];    /* grey red yellow blue magenta cyan white */
    bool pure_use_rprompt;
    char branch_icon[24];      /* the mode's VCS_BRANCH_ICON, as icons.zsh writes it */
    char instant_prompt[16];
    char options[1024];        /* the header's words, \037 between them */
    bool overwrite;            /* the old configuration is to be kept aside first */
};

enum wizard_end { WIZARD_DONE, WIZARD_QUIT };

/* Ask, page by page, until every question is answered (WIZARD_DONE, the
 * answers in `a`, "New config: ..." said) or the person quits -- q, Ctrl+C,
 * the input gone (WIZARD_QUIT, how to run it again said). Uses the alternate
 * screen and leaves it. */
enum wizard_end wizard_run(const struct wizard_io *io, const struct wizard_env *env, struct wizard_answers *a);

/* The template the configuration is made from: "lean-8colors" for lean_8colors. */
void wizard_template(const struct wizard_answers *a, char *out, size_t cap);

/* The configuration: `base`, the template's text, changed as the answers say,
 * under the header (`date` as p10k writes it, "2026-10-26 at 15:02 UTC").
 * malloc'd; NULL when out of memory. */
char *wizard_config(const struct wizard_answers *a, const char *base, const char *date);

#endif /* __EMBK_WIZARD_H__ */
