/* ==========================================================================
 * prompt.h -- the interactive prompt: its options and the text built from
 * them.
 *
 * OPTIONS are set as commands (`prompt set right status duration time`), in
 * ~/.config/shell/rc.esh or at the prompt; they live for the session. Keys are
 * dotted lower-case names, values text. The builder reads the ones it knows:
 *
 *   char                 the prompt character                     ($)
 *   right                what the right side shows, in order:     (none)
 *                        status, duration, time
 *   transient            off | always | same-dir: once a line is  (off)
 *                        accepted its prompt is drawn as `char`
 *                        alone (same-dir: only while the directory
 *                        stays the same)
 *   duration.threshold   seconds a command must run for its       (3)
 *                        duration to show
 *   time.format          24h | 12h                                (24h)
 *
 * POWERLEVEL10K. A style (`prompt style lean`) or a person's ~/.p10k.zsh
 * (`prompt import-p10k`) loaded as the base turns the prompt into
 * Powerlevel10k's (shell/prompt/p10k.h), drawn by its own parameters; every
 * option set is then one of them on top of the base, by its name in dotted
 * lower case -- dir.anchor.fg is POWERLEVEL9K_DIR_ANCHOR_FOREGROUND (fg and
 * bg for FOREGROUND and BACKGROUND). The five above keep their meaning
 * there (char, right, transient, duration.threshold, time.format). With no
 * base the prompt is the plain one above (docs/SHELL.md, "The prompt").
 *
 * PURE: no clock, no environment, no files -- the shell hands the builder
 * what it needs (struct prompt_state), so the host tests it.
 * ========================================================================== */
#ifndef __EMBK_PROMPT_H__
#define __EMBK_PROMPT_H__

#include "prompt/p10k.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROMPT_KEY_MAX   64
#define PROMPT_VALUE_MAX 256

/* Set, read, forget. A key is [a-z0-9_] words joined by single dots; set
 * returns false for any other, or when the table is full. */
bool prompt_opt_set(const char *key, const char *value);
const char *prompt_opt(const char *key);            /* NULL when unset */
bool prompt_opt_unset(const char *key);             /* false if it was not set */
void prompt_opt_reset(void);
size_t prompt_opt_count(void);                      /* in the order they were first set */
const char *prompt_opt_key(size_t i);
const char *prompt_opt_value(size_t i);
bool prompt_key_valid(const char *key);

/* What a prompt shows of the shell. */
struct prompt_state {
    const char *user, *host;
    const char *dir;               /* where the shell is, ~ for home */
    const char *last_dir;          /* where the last prompt was ("" before the first) */
    int status;                    /* the last command's exit status, -1 before the first */
    uint64_t duration_ms;          /* how long it ran */
    int hour, minute, second;      /* now */
    /* and for Powerlevel10k's segments */
    const char *pwd, *last_pwd, *home;     /* absolute */
    int year, month, mday, wday;   /* month 1-12, wday 0 = Sunday */
    int jobs;                      /* background jobs */
    bool root;
    const struct p10k_git *git;    /* NULL: not in a repository */
    const struct p10k_fs *fs;      /* NULL: no listings (no unique prefixes, no markers) */
};

/* The prompt for a row of `cols` columns. `left` and `right` may carry SGR;
 * `transient` is empty when the transient prompt is off. `left` may be
 * several lines, \r\n between them; the right side goes on the last. */
struct prompt_text { char left[4096], right[1024], transient[512]; };
void prompt_build(const struct prompt_state *s, int cols, struct prompt_text *out);

/* --- Powerlevel10k ------------------------------------------------------- */

/* Load `text` (a .p10k.zsh) as the base; `source` names it (a style, a
 * path). False, and the prompt as it was, when it does not read. */
bool prompt_p10k_load(const char *source, const char *text, struct p10k_import *report);
void prompt_p10k_drop(void);                 /* back to the plain prompt */
const char *prompt_p10k_source(void);        /* the base's name, or NULL */
/* An option's parameter: "dir.anchor.fg" -> "DIR_ANCHOR_FOREGROUND". */
void prompt_p10k_name(const char *key, char *out, size_t cap);
/* The configuration the prompt is drawn from now: the base and the options
 * (NULL with no base). */
const struct p10k *prompt_p10k_config(void);

#endif /* __EMBK_PROMPT_H__ */
