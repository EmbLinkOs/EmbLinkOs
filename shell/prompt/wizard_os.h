/* wizard_os.h -- `prompt configure` on the terminal: Powerlevel10k's wizard
 * (shell/prompt/wizard.h) on fds 0 and 1, and what it writes:
 *
 *   ~/.config/shell/p10k.zsh    the configuration, as p10k configure writes it
 *   ~/.config/shell/prompt.esh  what loads it, read before rc.esh
 *
 * TARGET-ONLY (embk syscalls). */
#ifndef __EMBK_WIZARD_OS_H__
#define __EMBK_WIZARD_OS_H__

#include <stdbool.h>
#include <stddef.h>

/* Ask, write the configuration and load it. `force`: typed (`prompt
 * configure`); false: a shell that found no prompt configuration runs it,
 * and says nothing when it cannot. 1: configured; 0: quit; -1: it cannot run
 * here (a terminal too small, none at all, no home), and `err` says why. */
int wizard_os_run(bool force, char *err, size_t cap);

/* Whether a shell starting now runs it: no prompt configuration -- no style
 * or file loaded, no option set -- and no EMBLINK_NO_WIZARD; and not a Live
 * session, which is for trying the system and skips its first-run setup the
 * same way. */
bool wizard_os_wanted(void);

#endif
