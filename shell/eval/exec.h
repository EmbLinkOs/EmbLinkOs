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

/* Has a human asked this program to stop? Checked between statements and at
 * the top of every loop iteration.
 *
 * The shell provides this rather than the executor calling the OS directly,
 * because the host test build has no OS to ask -- and because whether ^C is
 * even AIMED at this process is the driver's business, not the executor's. */
void exec_set_interrupt_check(bool (*fn)(void));

/* A `def` stores a BORROWED pointer into the parsed AST, so the AST has to
 * outlive it. The driver hands the parsed program here INSTEAD of freeing it
 * when it contained a definition. Returns false if the retain table is full
 * (the definition still works; the caller should report it). */
bool exec_retain_program(struct block *b);

/* --- background jobs -------------------------------------------------------
 *
 * `pipeline &` spawns ANOTHER SHELL to run that pipeline's source text and
 * records it here. Spawning a shell rather than the program directly is what
 * lets a BUILTIN pipeline go to the background at all: builtins run
 * in-process, and this process is busy being the shell.
 *
 * A job is remembered by its spawn HANDLE, which is the only name a parent has
 * for a child in this OS -- never a pid, so a recycled pid cannot alias. */
#define JOB_MAX 32

struct job {
    int   id;            /* 1-based, as displayed; 0 = free slot */
    int   handle;        /* the spawn handle */
    char *cmd;           /* the source text, for `jobs` to show */
    bool  reaped;        /* it has exited AND been waited for */
    int   status;        /* its exit code, once reaped */
    /* STOPPED IS NOT FINISHED. A job frozen by ^Z or by `stop` still holds its
     * memory, its fds and this handle; it simply is not being scheduled. It
     * must never be waited for in that state -- a stopped process is not going
     * to exit -- which is why `fg` resumes before it waits. */
    bool  stopped;
};

/* Start one. Returns the job id, or a negative errno. */
int  jobs_start(const char *src);

/* Take an ALREADY-RUNNING child into the job table, marked stopped.
 *
 * This is what ^Z on a foreground command needs: the child was spawned as an
 * ordinary foreground process, and only became a job when someone froze it.
 * The handle transfers -- the caller must not wait on or close it afterwards.
 * Returns the job id, or a negative errno (and the caller still owns the
 * handle if it fails). */
int  jobs_adopt_stopped(int handle, const char *src);

/* Freeze a job, or let it go again. `bg` is resume-and-do-not-wait; `fg`
 * resumes first and then waits, because waiting on a stopped process waits for
 * an exit that is not coming. */
int  jobs_stop(struct job *j);
int  jobs_resume(struct job *j);

/* How many slots are in use, and the i'th of them (NULL past the end).
 * `jobs_poll()` first: it notices finished children and marks them. */
void        jobs_poll(void);
size_t      jobs_count(void);
struct job *jobs_at(size_t i);
struct job *jobs_by_id(int id);

/* Wait for a job to finish, returning its exit status.
 *
 * Resumes it first if it was stopped. Returns -EMBK_ESTOPPED if it stops again
 * while being waited for (someone pressed ^Z), in which case the job is left
 * in the table marked stopped rather than reaped. */
int jobs_wait(struct job *j);

/* Free a finished job's slot, after its exit status has been reported once. */
void jobs_forget(struct job *j);

/* The user-command table, for `help` and completion. */
bool        exec_is_user_command(const char *name);
size_t      exec_user_command_count(void);
const char *exec_user_command_name(size_t i);

/* Set by exec_init; read by userfn_call. */
value_sink    exec_sink(void);
void         *exec_sink_ctx(void);
struct scope *exec_top_scope(void);

#endif /* __EMBK_EXEC_H__ */
