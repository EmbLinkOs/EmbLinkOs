/* p10k_int.h -- what the prompt engine's parts share (p10k.c, p10k_zsh.c,
 * p10k_render.c, p10k_import.c). Not for the rest of the shell. */
#ifndef __EMBK_P10K_INT_H__
#define __EMBK_P10K_INT_H__

#include "prompt/p10k.h"
#include <stdarg.h>

/* A growing string. Out of memory is sticky (oom) and leaves it as it was. */
struct sb { char *p; size_t n, cap; bool oom; };
void sb_add(struct sb *b, const char *s, size_t n);
void sb_str(struct sb *b, const char *s);
void sb_chr(struct sb *b, char c);
void sb_fmt(struct sb *b, const char *fmt, ...);
const char *sb_text(const struct sb *b);
void sb_free(struct sb *b);
void sb_reset(struct sb *b);

/* Powerlevel10k's lookups. `seg` is a segment's state name as p10k spells
 * it ("prompt_dir_HOME", "prompt_prompt_char_OK_VIINS", "" for none):
 * _p9k_param tries <SEG>_<STATE>_<NAME>, <SEG>_<NAME>, <NAME>, the default. */
const char *p9k_param(const struct p10k *cfg, const char *seg, const char *name, const char *def);
bool p9k_isset(const struct p10k *cfg, const char *name);
/* _p9k_get_icon: the parameter, else the mode's icon, else `def`; escapes
 * (✔) decoded. Into `out`. */
const char *p9k_icon(const struct p10k *cfg, const char *seg, const char *name, const char *def, struct sb *out);
/* _p9k_color: the parameter translated -- "076", "#aabbcc", or "" (none). */
const char *p9k_color(const struct p10k *cfg, const char *seg, const char *name, const char *def, char out[16]);
void p9k_translate_color(const char *c, char out[16]);
/* Typed reads of a whole parameter, with p10k's own defaults. */
bool p9k_bool(const struct p10k *cfg, const char *name, bool def);
long p9k_int(const struct p10k *cfg, const char *name, long def);
double p9k_float(const struct p10k *cfg, const char *name, double def);
/* An array parameter's i-th element (by P10K_ARRAY_SEP, or spaces). */
size_t p9k_array(const char *value, size_t i, char *out, size_t cap);

/* A zsh pattern matched against all of `s`. p9k_glob_name matches a file
 * name as a glob does: a leading dot only by a dot. */
bool p9k_glob(const char *pat, size_t pn, const char *s, size_t sn);
bool p9k_glob_name(const char *pat, const char *name);

/* ${(g::)...}: ✔, \x41, \n, \e and the like. */
void p9k_decode_escapes(const char *s, struct sb *out);
/* A Powerlevel10k *_EXPANSION value evaluated: ${P9K_CONTENT},
 * ${P9K_VISUAL_IDENTIFIER}, their ${x/pat/rep} and ${x//pat/rep}, literal
 * text, and the wizard's my_git_formatter. */
void p9k_expand_param(const struct p10k *cfg, const char *exp, const char *content, const char *icon,
                      const struct p10k_state *st, struct sb *out);
/* MULTILINE_*_PROMPT_GAP_EXPANSION with ${P9K_GAP} the filler. */
void p9k_expand_gap(const char *exp, const char *gap, struct sb *out);
/* my_git_formatter, as every wizard-written configuration defines it. */
void p9k_git_formatter(const struct p10k *cfg, const struct p10k_state *st, bool loading, struct sb *out);

/* The zsh prompt string to SGR, and its width. */
void p9k_zexpand(const char *z, const struct p10k_state *st, struct sb *out);

#endif
