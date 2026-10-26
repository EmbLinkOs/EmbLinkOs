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
 * The names follow Powerlevel10k's parameters (POWERLEVEL9K_TRANSIENT_PROMPT,
 * ..._COMMAND_EXECUTION_TIME_THRESHOLD), so a configuration reads the same
 * way, and options the builder does not know yet are kept for those that
 * will (docs/SHELL.md, "The prompt").
 *
 * PURE: no clock, no environment, no files -- the shell hands the builder
 * what it needs (struct prompt_state), so the host tests it.
 * ========================================================================== */
#ifndef __EMBK_PROMPT_H__
#define __EMBK_PROMPT_H__

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
};

/* The prompt for a row of `cols` columns. `left` and `right` may carry SGR;
 * `transient` is empty when the transient prompt is off. */
struct prompt_text { char left[1024], right[256], transient[128]; };
void prompt_build(const struct prompt_state *s, int cols, struct prompt_text *out);

#endif /* __EMBK_PROMPT_H__ */
