/* ==========================================================================
 * complete.h -- what Tab offers at the prompt.
 *
 * The line editor (shell/edit) asks; this answers from what the SHELL knows:
 * its builtins and keywords and the commands defined with `def` where a
 * command goes, `$` names where a variable goes, and files and directories
 * everywhere else -- the same programs the shell would actually run
 * (/system/bin, /data/apps/<name>), not a list kept somewhere else that
 * drifts. Names the lexer would not read as one word are offered in quotes.
 * ========================================================================== */
#ifndef __EMBK_COMPLETE_H__
#define __EMBK_COMPLETE_H__

#include <stddef.h>
#include "edit/edit.h"

struct scope;

/* An edit_host.complete: `ctx` is the session's top scope (for $names). */
void shell_complete(void *ctx, const char *line, size_t cursor, struct edit_matches *out);

#endif /* __EMBK_COMPLETE_H__ */
