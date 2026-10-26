/* prompt_os.h -- what Powerlevel10k's segments ask of the system: the
 * filesystem questions of the directory segment (p10k_fs) and the git
 * repository a directory is in. TARGET-ONLY (embk syscalls). */
#ifndef __EMBK_PROMPT_OS_H__
#define __EMBK_PROMPT_OS_H__

#include "prompt/p10k.h"

/* Listings answer the questions, one per directory, kept for one prompt:
 * prompt_os_fresh() before building the next, so it looks again. */
extern const struct p10k_fs prompt_os_fs;
void prompt_os_fresh(void);

/* The repository `pwd` is in: its branch (or the commit, detached). The
 * counts are gitstatus's (M10); until it answers, `loading`. False: not in
 * one. */
bool prompt_os_git(const char *pwd, struct p10k_git *g);

#endif
