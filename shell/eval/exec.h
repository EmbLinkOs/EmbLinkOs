/* ==========================================================================
 * exec.h -- running a PROGRAM. See exec.c for the reasoning.
 * ========================================================================== */
#ifndef __EMBK_EXEC_H__
#define __EMBK_EXEC_H__

#include "parse/parse.h"
#include "eval/eval.h"
#include "value/value.h"
#include <stdbool.h>

/* Where control went. `break`/`continue` unwind to the nearest loop and
 * `return` to the nearest command body; a value that is still FLOW_BREAK when
 * it reaches the top level is a break outside a loop, which the driver
 * reports rather than silently ignoring. */
enum exec_flow { FLOW_NORMAL = 0, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN };

struct exec_out {
    enum exec_flow flow;
    struct value   value;    /* the last statement's value; OWNED by the caller */
    bool           error;
};

/* What to do with each statement's value: print it, emit it into a structured
 * pipe, or nothing. The DRIVER decides -- the executor must not know whether a
 * human is watching. */
typedef void (*value_sink)(struct value *v, void *ctx);

/* Wire up the session: the top-level scope user commands resolve against, and
 * the sink their bodies' statements print through. Must be called before any
 * program runs. */
void exec_init(struct scope *top, value_sink sink, void *sink_ctx);

/* Run every statement of `b` in `env`. Returns 0, or -1 with out->value
 * holding a VAL_ERROR. `out` must be initialised (value = value_null()) by
 * the caller and freed by it afterwards. */
int block_exec(const struct block *b, struct scope *env,
               value_sink sink, void *ctx, struct exec_out *out);

/* A `def` stores a BORROWED pointer into the parsed AST, so the AST has to
 * outlive it. The driver hands the parsed program here INSTEAD of freeing it
 * when it contained a definition. Returns false if the retain table is full
 * (the definition still works; the caller should report it). */
bool exec_retain_program(struct block *b);

/* The user-command table, for `help` and completion. */
bool        exec_is_user_command(const char *name);
size_t      exec_user_command_count(void);
const char *exec_user_command_name(size_t i);

/* Set by exec_init; read by userfn_call. */
value_sink    exec_sink(void);
void         *exec_sink_ctx(void);
struct scope *exec_top_scope(void);

#endif /* __EMBK_EXEC_H__ */
