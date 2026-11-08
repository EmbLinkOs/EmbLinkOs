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

/* The repository `pwd` is in, from .git/HEAD alone: its branch (or the
 * commit, detached), `loading`. False: not in one. */
bool prompt_os_git(const char *pwd, struct p10k_git *g);

/* The repository's status from /system/bin/gitstatus.elf, the git status
 * daemon (one per shell, started on first need with the configuration's
 * VCS_*_MAX_NUM, VCS_MAX_INDEX_SIZE_DIRTY and VCS_RECURSE_UNTRACKED_DIRS), as
 * Powerlevel10k asks gitstatusd: an answer within VCS_MAX_SYNC_LATENCY_SECONDS
 * fills `g`; a later one leaves `g` the last status known for the repository
 * (or nothing yet) in the loading state, and arrives through
 * prompt_os_git_answer(). Without the daemon, .git/HEAD as above. */
bool prompt_os_gitstatus(const struct p10k *cfg, const char *pwd, struct p10k_git *g);
/* The fd an awaited answer will arrive on, or -1 when none is awaited. */
int prompt_os_git_waiting(void);
/* The awaited answer, if it is complete: into `g`, and true. */
bool prompt_os_git_answer(const struct p10k *cfg, struct p10k_git *g);

#endif
