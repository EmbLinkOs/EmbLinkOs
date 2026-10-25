/* sh_complete.h -- what Tab offers at the sh prompt (an edit_host.complete).
 * POSIX words: a command where one goes (sh builtins, the port's utilities,
 * /system/bin and /data/apps), files elsewhere, $names after $; special
 * characters are escaped with backslashes, as a POSIX shell reads them. */
#ifndef EMBLINK_SH_COMPLETE_H
#define EMBLINK_SH_COMPLETE_H
#include <stddef.h>
#include "edit/edit.h"

struct mrsh_state;
void sh_complete(void *state, const char *line, size_t cursor, struct edit_matches *out);
#endif
