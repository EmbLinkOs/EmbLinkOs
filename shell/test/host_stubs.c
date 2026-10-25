/* Host-build stand-ins for the OS-backed pieces (builtins_os.c and
 * eval_extern.c are target-only: embk syscalls). The pure pipeline is fully
 * testable without them; OS command names simply don't resolve here and
 * fall through to the unknown-command error below. */
#include "builtins/builtins.h"
#include "eval/eval.h"
#include <stdio.h>

builtin_fn builtin_lookup_os(const char *name) {
    (void)name;
    return NULL;   /* ls/cat/ps/... : not on the host */
}

struct value extern_stage_run(const struct command *cmd, struct value input,
                              struct scope *env, bool to_terminal) {
    (void)env; (void)to_terminal;
    value_free(&input);
    char msg[96];
    snprintf(msg, sizeof msg, "unknown command '%s' (externals need the OS)", cmd->name);
    return value_error(msg);
}

/* Files are the OS's: a redirected builtin says so on the host. */
struct value stage_redirect_input(const struct command *cmd, struct value input, struct scope *env) {
    (void)cmd; (void)env;
    value_free(&input);
    return value_error("redirections need the OS");
}
struct value stage_redirect_output(const struct command *cmd, struct value result, struct scope *env) {
    (void)cmd; (void)env;
    value_free(&result);
    return value_error("redirections need the OS");
}
