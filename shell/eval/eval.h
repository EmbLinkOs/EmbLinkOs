/* ==========================================================================
 * eval.h -- the two evaluators (the sketch's structural split):
 *
 *   - PIPELINE evaluator: runs a sequence of commands, threading a
 *     materialized Value from each stage into the next (v1 = materialize,
 *     not stream: a stage is `Value transform(Value)`; fine at shell scale).
 *   - EXPRESSION evaluator: evaluates one expr AST to a single Value in a
 *     scope. Called BY commands (where/sort-by) once per row with the row's
 *     fields in scope.
 *
 * SETTLED decisions (were open in the sketch; rationale in docs/SHELL.md):
 *   - TRUTHINESS IS STRICT: Bool is itself, Null is false, anything else in
 *     a boolean context is an ERROR ("expected a boolean"), not a guess.
 *     A structured shell earns its keep by being explicit.
 *   - ERROR MODEL: errors are VAL_ERROR values threaded through the pipeline;
 *     the first one aborts the remaining stages and the driver prints it and
 *     keeps the prompt alive. No separate channel, no early-abort longjmp.
 *   - Bare words (EXPR_COLUMN) resolve scope-first, then FALL BACK to their
 *     own name as a STRING -- so `echo hi` and `ls /data` read naturally
 *     without quoting, while `where size > 1mb` still resolves `size` from
 *     the row. $vars never fall back: an unresolved $x is an error.
 *
 * COERCION lives HERE (the value layer deliberately refused it):
 *   - the numeric family (INT / FILESIZE / DATE / FLOAT) is mutually
 *     comparable; FLOAT contaminates to double, otherwise int64 compare.
 *   - STRING and PATH are mutually comparable text.
 *   - arithmetic keeps the "richer" tag: INT+FILESIZE = FILESIZE,
 *     INT+DATE = DATE, FLOAT anywhere = FLOAT; FILESIZE+DATE is an error.
 *   - `=~` is substring-match over text (real regex is future work).
 * ========================================================================== */
#ifndef __EMBK_EVAL_H__
#define __EMBK_EVAL_H__

#include "parse/parse.h"
#include "value/value.h"

/* -------------------------------------------------------------------------
 * Scope: lexical chain + the current row. `let` bindings first, then the
 * row's fields, then the parent -- one lookup path for columns and $vars
 * (the parser's EXPR_COLUMN/EXPR_VAR split only sharpens error messages).
 * ------------------------------------------------------------------------- */
struct scope {
    struct scope        *parent;   /* NULL at the top */
    const struct record *row;      /* borrowed; NULL when no row context */
    char                **names;   /* owned `let` binding names  */
    struct value         *values;  /* owned `let` binding values */
    size_t                count, cap;
};

void scope_init(struct scope *s, struct scope *parent);
void scope_free(struct scope *s);                    /* frees bindings, not parent */
int  scope_bind(struct scope *s, const char *name, struct value v);
     /* MOVES v in; replaces an existing binding of the same name. 0 / -1 OOM. */
const struct value *scope_lookup(const struct scope *s, const char *name);
     /* bindings -> row field -> parent; NULL if nowhere. BORROWED. */

/* -------------------------------------------------------------------------
 * Expression evaluation -> an OWNED value (possibly VAL_ERROR).
 * ------------------------------------------------------------------------- */
struct value expr_eval(const struct expr *e, struct scope *env);

/* Strict truthiness (see header comment). Returns 0/1, or -1 if the value
 * isn't boolean-shaped -- callers turn that into a VAL_ERROR. */
int value_truthy(const struct value *v);

/* Ordering WITH the evaluator's coercion (numeric family cross-compares,
 * text family cross-compares). 0 -> *out is -1/0/1; -1 -> no meaningful
 * order (caller builds the error). What sort-by keys on. */
int eval_compare_values(const struct value *a, const struct value *b, int *out);

/* Convenience for builtins that take a WORD argument (a path, a column
 * name): an EXPR_COLUMN's name or a literal STRING/PATH, WITHOUT evaluating
 * it against the scope. Returns a BORROWED char* or NULL if the expr isn't
 * word-shaped. */
const char *expr_as_word(const struct expr *e);

/* -------------------------------------------------------------------------
 * Pipeline evaluation. `top` is the driver's persistent scope (lets live
 * there across REPL lines). Returns the final OWNED value -- VAL_ERROR if
 * any stage failed (remaining stages are skipped).
 *
 * `to_terminal`: the pipeline is a statement, so whatever its LAST stage
 * prints is for a person -- a text program there keeps the terminal (an
 * editor, `sh`, anything interactive works). False for `$(...)`, whose value
 * is the point: a text program at its end has its output captured.
 * ------------------------------------------------------------------------- */
struct value pipeline_run(const struct pipeline *pl, struct scope *top, bool to_terminal);

/* One external stage: resolve the program, spawn it with the plumbing its
 * kind wants -- a program that speaks the wire (it links the shell SDK, see
 * sval.h) gets fd 3 for structured output and frames on fd 0; a text program
 * gets the input as text and, unless it ends a statement, has its stdout
 * captured as a string -- apply its redirections, reap it.
 * Implemented in eval_extern.c ON TARGET; the host test build links a stub
 * that returns an error ("external programs need the OS"). Takes ownership
 * of `input`. */
struct value extern_stage_run(const struct command *cmd, struct value input,
                              struct scope *env, bool to_terminal);

/* What a command name runs (eval_extern.c, target only): 0 and the program's
 * absolute path, or -1 when there is none. A name with a '/' is a path, made
 * absolute against the cwd; a bare name is looked for as
 * /system/bin/NAME.elf, /system/bin/NAME, then /data/apps/NAME/NAME.elf. */
int shell_resolve_program(const char *name, char *path, size_t cap);
/* What kind of program a file is: an ELF that carries the SDK's mark speaks
 * the wire; any other ELF is a text program; a `#!` script is the kind of
 * its interpreter. */
enum { PROGRAM_TEXT = 0, PROGRAM_WIRE, PROGRAM_SCRIPT, PROGRAM_SCRIPT_WIRE };
int shell_program_kind(const char *path);

/* Redirections of a builtin or a defined command (eval_extern.c on target):
 * `in<` turns a file into the stage's input (a string); after the stage,
 * `out>`/`out>>` write its value as text to a file and leave null behind,
 * `err>`/`err>>` write an error's message instead of failing the pipeline.
 * Both take ownership of the value they are given. */
struct value stage_redirect_input(const struct command *cmd, struct value input, struct scope *env);
struct value stage_redirect_output(const struct command *cmd, struct value result, struct scope *env);

/* -------------------------------------------------------------------------
 * USER-DEFINED COMMANDS, reached through a hook rather than a direct call.
 *
 * `def name { ... }` lives in the statement layer (eval/exec.c), which sits
 * ABOVE this one: exec.c calls pipeline_run, so pipeline_run calling exec.c
 * back by name would be a cycle -- and the host test build links eval.c
 * WITHOUT exec.c at all. Two function pointers, NULL until exec_init() fills
 * them, keep the layering honest and keep a build with no program layer
 * behaving exactly as it did before.
 *
 * Resolution order is builtin, then user command, then external program; the
 * reasoning is in exec.c's header.
 * ------------------------------------------------------------------------- */
extern bool (*eval_user_command_exists)(const char *name);
extern struct value (*eval_user_command_call)(const struct command *cmd,
                                              struct value input,
                                              struct scope *env);

#endif /* __EMBK_EVAL_H__ */
