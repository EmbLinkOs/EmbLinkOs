/* ==========================================================================
 * p10k.h -- a Powerlevel10k-compatible prompt engine.
 *
 * A configuration is Powerlevel10k's parameters by their own names (without
 * the POWERLEVEL9K_ prefix: DIR_ANCHOR_FOREGROUND, LEFT_PROMPT_ELEMENTS),
 * set one at a time or read from a ~/.p10k.zsh (p10k_import). The engine draws
 * what Powerlevel10k draws for them -- the same segments, separators, frames,
 * gap, colours and shortening -- so a person's configuration renders the same
 * prompt here as on their Mac. tools/golden_prompt.py holds it to recordings
 * of Powerlevel10k itself (tests/prompt/golden).
 *
 * The engine builds zsh prompt strings the way Powerlevel10k does (%F{39},
 * %K{238}, %B, %{...%}) and expands them to SGR at the end, so its logic can
 * follow p10k's line for line. PURE: the shell hands it a p10k_state -- the
 * directory, the last status, the clock, git -- and answers its few
 * questions about the filesystem through p10k_fs.
 * ========================================================================== */
#ifndef __EMBK_P10K_H__
#define __EMBK_P10K_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- the configuration ----------------------------------------------------- */

struct p10k;
struct p10k *p10k_new(void);
void p10k_free(struct p10k *cfg);
void p10k_clear(struct p10k *cfg);
/* `name` without POWERLEVEL9K_. An array's elements are joined by \037; a
 * value without \037 that an array is read from is split at spaces. */
bool p10k_set(struct p10k *cfg, const char *name, const char *value);
bool p10k_unset(struct p10k *cfg, const char *name);
const char *p10k_get(const struct p10k *cfg, const char *name);   /* NULL: unset */
size_t p10k_count(const struct p10k *cfg);
const char *p10k_name_at(const struct p10k *cfg, size_t i);
const char *p10k_value_at(const struct p10k *cfg, size_t i);
#define P10K_ARRAY_SEP '\037'

/* A ~/.p10k.zsh (or a style's template): its `typeset -g POWERLEVEL9K_...`
 * lines, the conditions they sit in, and the git formatter it defines. */
struct p10k_import {
    unsigned set;              /* parameters set */
    unsigned line;             /* where it stopped, on an error */
    char error[160];
    char ignored[512];         /* segments it names that draw nothing here */
};
bool p10k_import(struct p10k *cfg, const char *text, struct p10k_import *report);

/* The segments the engine draws, and those it knows and draws nothing for
 * (direnv, asdf, kubecontext...: tools this system does not have). */
bool p10k_segment_known(const char *name);
bool p10k_segment_drawn(const char *name);

/* --- what a prompt is drawn from ------------------------------------------- */

struct p10k_git {
    bool repo, loading;
    char branch[128], tag[128], commit[48], remote_branch[128], remote_url[256], action[48];
    char summary[128];         /* the last commit's subject */
    int staged, unstaged, untracked, conflicted;
    int ahead, behind, push_ahead, push_behind, stashes;
    int has_unstaged;          /* -1: not known (the index was too big to look) */
};

struct p10k_fs {
    void *ctx;
    /* Call `each` for every entry of directory `dir` but . and .. (`dir`:
     * it is one). False when it cannot be read. The directory segment asks
     * it of the directories on the way to where the shell is: which names a
     * prefix leaves unique (truncate_to_unique), which hold a folder marker
     * (.git). */
    bool (*list)(void *ctx, const char *dir, void (*each)(void *arg, const char *name, bool dir), void *arg);
    bool (*writable)(void *ctx, const char *dir);
    bool (*exists)(void *ctx, const char *dir);
};

struct p10k_state {
    const char *pwd, *home, *user, *host;
    bool root, ssh, sudo;
    int status;                /* the last command's exit status; -1 before the first */
    double duration;           /* how long it ran, seconds; < 0: not known */
    int jobs;                  /* background jobs */
    int year, month, mday, wday, hour, minute, second;   /* now, local; month 1-12, wday 0 = Sunday */
    const char *os;            /* the os_icon's: "macos", "linux", "android", "windows",
                                * "freebsd", "solaris"; another (EmbLinkOS) has none */
    const char *last_prompt_pwd;   /* where the last prompt was: transient same-dir */
    bool first;                /* the first prompt of the terminal (p10k's P9K_TTY=new):
                                * no empty line, no ruler */
    struct p10k_git git;
    const struct p10k_fs *fs;  /* NULL: no shortening by uniqueness, no markers */
};

/* --- the prompt ---------------------------------------------------------- */

struct p10k_prompt {
    char *left;                /* every line, with SGR, "\n" between them: the
                                * last one's left part last (PROMPT) */
    char *right;               /* the last line's right part (RPROMPT), or "" */
    char *transient;           /* the prompt an accepted line keeps, or "" (off) */
    int add_newline;           /* empty lines above it (PROMPT_ADD_NEWLINE, _COUNT),
                                * after the first prompt */
    int lines;
};
/* The prompt for a row of `cols` columns. False only when out of memory. */
bool p10k_render(const struct p10k *cfg, const struct p10k_state *st, int cols, struct p10k_prompt *out);
void p10k_prompt_free(struct p10k_prompt *p);

/* The cells a zsh prompt string takes (escapes and %{...%} take none). */
int p10k_width(const char *zprompt, const struct p10k_state *st);

#endif /* __EMBK_P10K_H__ */
