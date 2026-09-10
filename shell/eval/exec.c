/* ==========================================================================
 * exec.c -- running a PROGRAM, not a line.
 *
 * The shell could always compose commands with '|'. What it could not do was
 * decide, repeat, or be extended, and that is the difference between a
 * launcher and a language. With thirty builtins and fifty-odd programs and no
 * way to branch, a user has eighty capabilities. With `if`, `while`, `for` and
 * `def` they have as many as they are willing to write down -- and, the part
 * that actually matters, they can add one WITHOUT rebuilding the OS.
 *
 * WHAT THIS FILE OWNS:
 *   - block_exec(): statements in order, with the loop/return control flow.
 *   - The user-command table that `def` fills and a pipeline stage consults.
 *
 * WHY THE FUNCTION TABLE IS REACHED THROUGH A HOOK. pipeline_run() lives in
 * eval.c and dispatches builtin-then-external; teaching it about user commands
 * directly would make eval.c depend on the statement layer above it, and the
 * host test build links eval.c WITHOUT this file. So eval.c holds two function
 * pointers that are NULL until exec_init() sets them, and a build with no
 * program layer behaves exactly as it did before -- which is what keeps the
 * existing parser and evaluator tests honest.
 *
 * NAME RESOLUTION, and it is a decision rather than an accident:
 *
 *     builtin  >  user command  >  external program
 *
 * Builtins win because they are the language's own verbs. `where`, `select`
 * and `sort-by` are as much a part of the grammar as `if` is, and a script
 * that shadowed one would change the meaning of every pipeline that followed
 * it, including ones the author never read. User commands beat programs on
 * disk so that `def` can genuinely extend the system -- wrapping, replacing or
 * specialising a program is the whole point of being able to define one.
 * ========================================================================== */
#include "eval/exec.h"
#include "eval/eval.h"
#include "builtins/builtins.h"
#include "value/value.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "embk.h"        /* embk_spawn_env / embk_wait / embk_proc_alive */
#include <errno.h>

extern char **environ;

/* --- the user-command table ----------------------------------------------
 * A flat array, searched linearly. A shell session defines tens of commands,
 * not thousands, and a hash table here would be more code than the thing it
 * indexes. Redefining a name REPLACES it -- re-running a script must not
 * accumulate copies of its own definitions. */
struct userfn {
    char               *name;
    const struct stmt  *def;    /* BORROWED: the AST outlives the table --
                                 * see exec_retain_program(). */
};

#define USERFN_MAX 128
static struct userfn g_fns[USERFN_MAX];
static size_t        g_nfns = 0;

/* Every program whose AST a definition points into. `def` stores a BORROWED
 * pointer into the parsed block, so the block must outlive the definition --
 * a script that defines a command and exits its parse scope would otherwise
 * leave the table pointing at freed memory. The driver hands ownership here
 * instead of freeing, and the ASTs live for the session.
 *
 * That is a deliberate leak with a bounded shape: one AST per script or REPL
 * line that contained a `def`, held for the life of the shell. Copying the
 * body instead would mean a deep AST clone for a saving nothing measures. */
#define RETAINED_MAX 256
static struct block *g_retained[RETAINED_MAX];
static size_t        g_nretained = 0;

static struct userfn *userfn_find(const char *name) {
    for (size_t i = 0; i < g_nfns; i++)
        if (strcmp(g_fns[i].name, name) == 0)
            return &g_fns[i];
    return NULL;
}

static int userfn_define(const char *name, const struct stmt *def) {
    struct userfn *slot = userfn_find(name);
    if (slot) { slot->def = def; return 0; }     /* redefinition replaces */
    if (g_nfns == USERFN_MAX) return -1;
    char *copy = strdup(name);
    if (!copy) return -1;
    g_fns[g_nfns].name = copy;
    g_fns[g_nfns].def  = def;
    g_nfns++;
    return 0;
}

bool exec_retain_program(struct block *b) {
    if (g_nretained == RETAINED_MAX)
        return false;
    g_retained[g_nretained++] = b;
    return true;
}

bool exec_is_user_command(const char *name) {
    return userfn_find(name) != NULL;
}

size_t exec_user_command_count(void) { return g_nfns; }

const char *exec_user_command_name(size_t i) {
    return i < g_nfns ? g_fns[i].name : NULL;
}

/* --- calling one -----------------------------------------------------------
 *
 * ARGUMENTS ARE EXPRESSIONS, evaluated in the CALLER's scope, and bound to the
 * parameter names in a fresh child scope. Evaluating them in the caller's
 * scope is the only choice that makes `greet $name` mean what it reads;
 * evaluating them inside the callee would resolve `$name` against the
 * callee's own bindings, which is dynamic scoping and a well-documented way
 * to make a script's behaviour depend on where it was called from.
 *
 * The child's parent is the TOP-LEVEL scope, not the caller's, for the same
 * reason: a command must see the session's globals and its own parameters,
 * and nothing else. Otherwise a caller's local `let n = 3` would silently
 * satisfy a callee that forgot to declare `n`.
 *
 * The RESULT is the value of the last pipeline in the body, or whatever
 * `return` carried. That makes a definition composable -- it can sit in the
 * middle of a pipeline like any other stage.
 */
static struct value userfn_call(const struct command *cmd, struct value input,
                                struct scope *env) {
    struct userfn *fn = userfn_find(cmd->name);
    if (!fn) {
        value_free(&input);
        return value_error("no such command");
    }

    const struct stmt *d = fn->def;
    if (cmd->nargs != d->u.def.nparams) {
        value_free(&input);
        char msg[128];
        snprintf(msg, sizeof msg, "%s takes %u argument(s), got %u",
                 cmd->name, (unsigned)d->u.def.nparams, (unsigned)cmd->nargs);
        return value_error(msg);
    }

    struct scope local;
    scope_init(&local, exec_top_scope());

    for (size_t i = 0; i < cmd->nargs; i++) {
        struct value v = expr_eval(cmd->args[i], env);   /* CALLER's scope */
        if (v.type == VAL_ERROR) {
            scope_free(&local);
            value_free(&input);
            return v;
        }
        if (scope_bind(&local, d->u.def.params[i], v) != 0) {
            value_free(&v);
            scope_free(&local);
            value_free(&input);
            return value_error("out of memory binding an argument");
        }
    }

    /* The pipeline's input reaches the body as `$in`. A command that ignores
     * it is a source; one that uses it is a filter. Naming it rather than
     * making it implicit means a reader can see which one they are looking
     * at. */
    if (scope_bind(&local, "in", input) != 0) {     /* MOVES input in */
        scope_free(&local);
        return value_error("out of memory binding $in");
    }

    /* NO SINK inside a command body. A command is a FUNCTION: its output is
     * its return value, and the CALLER decides what happens to it. Passing
     * the driver's print sink down instead printed everything twice -- once
     * from the body's last statement and once from the caller's -- and, worse,
     * made `mycmd | count` print the rows it was supposed to be counting.
     *
     * The cost, stated plainly: a body's intermediate statements produce no
     * output. `def f { echo a; echo b }` yields "b". That is what a function
     * does with intermediate expressions, and composability is worth more here
     * than the convenience of a body that narrates itself -- a pipeline stage
     * that printed would be unusable in a pipeline, which is the whole point
     * of being able to define one. */
    struct exec_out out = { FLOW_NORMAL, value_null(), false };
    int rc = block_exec(d->u.def.body, &local, NULL, NULL, &out);

    scope_free(&local);
    if (rc != 0 && out.value.type != VAL_ERROR) {
        value_free(&out.value);
        return value_error("command failed");
    }
    return out.value;
}

/* --- interruption --------------------------------------------------------- */
static bool (*g_interrupted)(void) = NULL;

void exec_set_interrupt_check(bool (*fn)(void)) { g_interrupted = fn; }

static bool interrupted(void) { return g_interrupted && g_interrupted(); }

/* --- the sink and the top scope, set by the driver ------------------------ */
static value_sink   g_sink     = NULL;
static void        *g_sink_ctx = NULL;
static struct scope *g_top     = NULL;

value_sink    exec_sink(void)     { return g_sink; }
void         *exec_sink_ctx(void) { return g_sink_ctx; }
struct scope *exec_top_scope(void){ return g_top; }

void exec_init(struct scope *top, value_sink sink, void *sink_ctx) {
    g_top      = top;
    g_sink     = sink;
    g_sink_ctx = sink_ctx;
    eval_user_command_exists = exec_is_user_command;
    eval_user_command_call   = userfn_call;
}

/* --- iterating whatever `for` was handed --------------------------------- */

/* How many items a value offers a `for` loop, and how to fetch one.
 *
 * A LIST yields its items and a TABLE yields its ROWS, because that is what
 * every pipeline in this shell produces: `ls` is a table, `get name` is a
 * list. Anything else yields ITSELF, once -- `for x in 5` running the body
 * once with x = 5 is more useful and far less surprising than an error, and
 * it makes a command that sometimes returns one row and sometimes many safe
 * to loop over without the caller checking which happened. */
static size_t seq_count(const struct value *v) {
    switch (v->type) {
    case VAL_LIST:  return v->u.list.count;
    case VAL_TABLE: return v->u.table ? v->u.table->count : 0;
    case VAL_NULL:  return 0;              /* nothing to walk, not an error */
    default:        return 1;
    }
}

static struct value seq_at(const struct value *v, size_t i) {
    switch (v->type) {
    case VAL_LIST:  return value_clone(&v->u.list.items[i]);
    case VAL_TABLE: return record_clone_value(&v->u.table->rows[i]);
    default:        return value_clone(v);
    }
}

/* --- running statements --------------------------------------------------- */

static int stmt_exec(const struct stmt *st, struct scope *env,
                     value_sink sink, void *ctx, struct exec_out *out);

int block_exec(const struct block *b, struct scope *env,
               value_sink sink, void *ctx, struct exec_out *out) {
    out->flow = FLOW_NORMAL;
    out->error = false;
    value_free(&out->value);
    out->value = value_null();

    for (size_t i = 0; i < b->n; i++) {
        /* BETWEEN statements, not only in loops: a long straight-line script
         * spawning one slow program after another is just as much a thing a
         * person wants to stop. */
        if (interrupted()) {
            value_free(&out->value);
            out->value = value_error("interrupted");
            out->error = true;
            return -1;
        }
        int rc = stmt_exec(b->stmts[i], env, sink, ctx, out);
        if (rc != 0) { out->error = true; return rc; }
        if (out->flow != FLOW_NORMAL)
            return 0;                       /* break/continue/return propagates */
    }
    return 0;
}

/* A condition must be a BOOLEAN. The evaluator's truthiness is strict by
 * design -- "a structured shell earns its keep by being explicit" -- and this
 * is the place that would be most tempting to loosen. `if $count` where count
 * is 0 has meant two different things in two different shells this decade;
 * refusing it means it never means the wrong one here. */
static int cond_eval(const struct expr *e, struct scope *env,
                     struct exec_out *out, bool *result) {
    struct value c = expr_eval(e, env);
    if (c.type == VAL_ERROR) {
        value_free(&out->value);
        out->value = c;
        return -1;
    }
    int t = value_truthy(&c);
    value_free(&c);
    if (t < 0) {
        value_free(&out->value);
        out->value = value_error("condition must be true or false");
        return -1;
    }
    *result = (t != 0);
    return 0;
}

static int stmt_exec(const struct stmt *st, struct scope *env,
                     value_sink sink, void *ctx, struct exec_out *out) {
    switch (st->kind) {

    case STMT_LET: {
        struct value v = expr_eval(st->u.let.expr, env);
        if (v.type == VAL_ERROR) {
            value_free(&out->value);
            out->value = v;
            return -1;
        }
        if (scope_bind(env, st->u.let.name, v) != 0) {
            value_free(&v);
            value_free(&out->value);
            out->value = value_error("out of memory");
            return -1;
        }
        return 0;
    }

    case STMT_PIPELINE: {
        /* `pipeline &` -- hand the whole statement to another shell and carry
         * on. The value of a backgrounded statement is its JOB ID, not the
         * pipeline's result: the result does not exist yet, and reporting the
         * id is what lets the next line say `fg 1`. */
        if (st->bg_src) {
            int id = jobs_start(st->bg_src);
            value_free(&out->value);
            if (id < 0) {
                out->value = value_error("could not start a background job");
                return -1;
            }
            out->value = value_int(id);
            if (sink) sink(&out->value, ctx);
            return 0;
        }

        struct value v = pipeline_run(st->u.pipe, env);
        if (v.type == VAL_ERROR) {
            value_free(&out->value);
            out->value = v;
            return -1;
        }
        /* The value of a statement is the value of its pipeline, and it is
         * ALSO what gets shown. Both: `let x = ...` needs the value, a script
         * that ends in `ls` needs the display, and a function body needs the
         * last one as its result. */
        value_free(&out->value);
        out->value = v;
        if (sink && v.type != VAL_NULL)
            sink(&out->value, ctx);
        return 0;
    }

    case STMT_IF: {
        bool c = false;
        if (cond_eval(st->u.iff.cond, env, out, &c) != 0) return -1;
        if (c)
            return block_exec(st->u.iff.then_b, env, sink, ctx, out);
        if (st->u.iff.else_b)
            return block_exec(st->u.iff.else_b, env, sink, ctx, out);
        return 0;
    }

    case STMT_WHILE: {
        /* THE ITERATION CEILING IS GONE. It existed because the shell could
         * not be interrupted: `while true { }` could only be ended by
         * resetting the machine, so the loop refused after ten million passes
         * rather than hang. The comment on it said "remove this the day the
         * shell can be interrupted, and not before" -- and that day is this
         * commit. A loop now runs exactly as long as it is asked to, and stops
         * when a human presses ^C, which is the correct behaviour and was
         * never available before. */
        for (;;) {
            if (interrupted()) {
                value_free(&out->value);
                out->value = value_error("interrupted");
                return -1;
            }

            bool c = false;
            if (cond_eval(st->u.wh.cond, env, out, &c) != 0) return -1;
            if (!c) break;

            int rc = block_exec(st->u.wh.body, env, sink, ctx, out);
            if (rc != 0) return rc;
            if (out->flow == FLOW_BREAK)    { out->flow = FLOW_NORMAL; break; }
            if (out->flow == FLOW_CONTINUE) { out->flow = FLOW_NORMAL; continue; }
            if (out->flow == FLOW_RETURN)   return 0;
        }
        return 0;
    }

    case STMT_FOR: {
        struct value seq = expr_eval(st->u.fr.seq, env);
        if (seq.type == VAL_ERROR) {
            value_free(&out->value);
            out->value = seq;
            return -1;
        }

        /* The sequence is evaluated ONCE, up front. A `for` whose collection
         * is re-evaluated per iteration is how a loop over `ls` ends up
         * walking a directory the loop body is modifying. */
        size_t n = seq_count(&seq);
        int rc = 0;
        for (size_t i = 0; i < n; i++) {
            if (interrupted()) {
                value_free(&out->value);
                out->value = value_error("interrupted");
                rc = -1;
                break;
            }
            struct value item = seq_at(&seq, i);
            if (scope_bind(env, st->u.fr.var, item) != 0) {
                value_free(&item);
                value_free(&out->value);
                out->value = value_error("out of memory binding the loop variable");
                rc = -1;
                break;
            }
            rc = block_exec(st->u.fr.body, env, sink, ctx, out);
            if (rc != 0) break;
            if (out->flow == FLOW_BREAK)    { out->flow = FLOW_NORMAL; break; }
            if (out->flow == FLOW_CONTINUE) { out->flow = FLOW_NORMAL; continue; }
            if (out->flow == FLOW_RETURN)   break;
        }
        value_free(&seq);
        return rc;
    }

    case STMT_DEF:
        if (userfn_define(st->u.def.name, st) != 0) {
            value_free(&out->value);
            out->value = value_error("too many defined commands");
            return -1;
        }
        return 0;

    case STMT_BREAK:    out->flow = FLOW_BREAK;    return 0;
    case STMT_CONTINUE: out->flow = FLOW_CONTINUE; return 0;

    case STMT_RETURN: {
        struct value v = value_null();
        if (st->u.ret) {
            v = expr_eval(st->u.ret, env);
            if (v.type == VAL_ERROR) {
                value_free(&out->value);
                out->value = v;
                return -1;
            }
        }
        value_free(&out->value);
        out->value = v;
        out->flow = FLOW_RETURN;
        return 0;
    }
    }
    return 0;
}

/* ==========================================================================
 * BACKGROUND JOBS
 *
 * `pipeline &` spawns ANOTHER SHELL to run that pipeline's source text.
 *
 * WHY A SHELL AND NOT THE PROGRAM. Backgrounding by spawning the named
 * program directly would only work for pipelines that are a single external
 * command -- `ls | where size > 1mb &` could not be backgrounded at all,
 * because builtins run IN THIS PROCESS and this process is busy being the
 * shell. Re-parsing the source in a child costs a millisecond and makes
 * backgrounding a property of any statement rather than a privilege that
 * external programs happen to have.
 *
 * A job is remembered by its spawn HANDLE. That is the only name a parent has
 * for a child in this OS -- never a pid, so a recycled pid cannot alias a job
 * that has already finished.
 * ========================================================================== */

static struct job g_jobs[JOB_MAX];
static int        g_next_job_id = 1;

static struct job *job_free_slot(void) {
    for (size_t i = 0; i < JOB_MAX; i++)
        if (g_jobs[i].id == 0)
            return &g_jobs[i];
    return NULL;
}

int jobs_start(const char *src) {
    if (!src || !*src)
        return -EINVAL;

    struct job *j = job_free_slot();
    if (!j)
        return -EMFILE;

    char *copy = strdup(src);
    if (!copy)
        return -ENOMEM;

    /* The child gets THIS shell's environment, named explicitly -- nothing is
     * inherited on EmbLink. It does NOT get the console interrupt route: ^C
     * belongs to whatever is in the foreground, and a background job that
     * stole it would make the next ^C stop the wrong thing. */
    char *argv[] = { (char *)"/system/bin/shell.elf", (char *)"-c", copy, NULL };
    int64_t h = embk_spawn_env("/system/bin/shell.elf", argv, environ, NULL, 0);
    if (h < 0) {
        free(copy);
        return (int)h;
    }

    j->id     = g_next_job_id++;
    j->handle = (int)h;
    j->cmd    = copy;
    j->reaped = false;
    j->status = 0;
    return j->id;
}

/* Notice finished children. Called before anything reads the table, so `jobs`
 * never reports a corpse as running -- and so a job that ended is REAPED
 * rather than left as a zombie holding a process slot. */
void jobs_poll(void) {
    for (size_t i = 0; i < JOB_MAX; i++) {
        struct job *j = &g_jobs[i];
        if (j->id == 0 || j->reaped)
            continue;
        if (!embk_proc_alive(j->handle)) {
            j->status = (int)embk_wait(j->handle);
            j->reaped = true;
        }
    }
}

size_t jobs_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < JOB_MAX; i++)
        if (g_jobs[i].id != 0) n++;
    return n;
}

struct job *jobs_at(size_t idx) {
    size_t n = 0;
    for (size_t i = 0; i < JOB_MAX; i++) {
        if (g_jobs[i].id == 0) continue;
        if (n++ == idx) return &g_jobs[i];
    }
    return NULL;
}

struct job *jobs_by_id(int id) {
    for (size_t i = 0; i < JOB_MAX; i++)
        if (g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

int jobs_wait(struct job *j) {
    if (!j || j->id == 0)
        return -EINVAL;
    if (!j->reaped) {
        j->status = (int)embk_wait(j->handle);
        j->reaped = true;
    }
    return j->status;
}

/* A finished job is forgotten once it has been REPORTED once -- the slot is
 * freed after `jobs` shows its exit status. Keeping it forever would fill the
 * table; dropping it silently would mean a job could finish between two
 * prompts and leave no trace that it ever ran. */
void jobs_forget(struct job *j) {
    if (!j || j->id == 0) return;
    free(j->cmd);
    j->cmd = NULL;
    j->id  = 0;
}
