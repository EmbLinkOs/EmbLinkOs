#ifndef __PROCESS_DEBUG_H__
#define __PROCESS_DEBUG_H__

/* The live-debugging kernel contract (EMBDBG_Specification.md §6), kernel-
 * internal side. The ABI that crosses to userspace is in include/debug_abi.h;
 * this is the session object and the two hooks the rest of the kernel calls:
 *
 *   debug_on_exception() — from isr_handler, BEFORE the panic path: a fault
 *     from a debugged thread parks it and wakes the debugger instead of
 *     halting the machine (§6.6).
 *   debug_notify_exit()  — from process_exit_self: turn a debuggee's exit into
 *     a DBG_EV_EXITED the debugger's sys_debug_wait returns.
 *
 * A software breakpoint is entirely debugger-side (§6.3): it plants 0xCC with
 * sys_debug_mem and keeps the byte. The kernel keeps NO breakpoint table; it
 * only delivers the stop and single-steps on resume. */

#include "process/process.h"           /* struct process/thread, wait_queue */
#include "arch/x86_64/syscall/syscall.h" /* struct regs — the frame layout */
#include "include/debug_abi.h"         /* embk_debug_event + constants */

struct debug_session {
    int used;
    struct process *target;            /* the debuggee */
    struct process *debugger;          /* who holds the session (only it may drive) */

    struct wait_queue debugger_wq;     /* debugger blocks here in sys_debug_wait */
    struct wait_queue target_wq;       /* the stopped/born-stopped target parks here */

    int stopped;                       /* a stop event is pending for the debugger */
    int exited;                        /* target has exited (event holds the status) */
    struct embk_debug_event event;     /* the pending event */

    struct thread *stopped_thread;     /* which thread is parked */
    struct regs   *stopped_frame;      /* its register frame (target's kstack), or
                                        * NULL when born-stopped (no frame yet) */
};

/* Called from process_create_caps when a spawn carries SPAWN_ACTION_DEBUG:
 * create a session with `debugger` as owner and `target` as debuggee, park the
 * target's (never-run) thread born-stopped. Returns the session or NULL. */
struct debug_session *debug_session_spawn(struct process *debugger,
                                          struct process *target,
                                          struct thread *target_thread);

/* isr_handler hook. Returns 1 if the fault belonged to a debug session and the
 * thread was parked + later resumed (the caller must `return`, i.e. iretq back
 * to ring 3); 0 if there is no session and the caller should fall through to
 * the panic path. Blocks the current (faulting) thread until resumed. */
int debug_on_exception(struct thread *t, struct regs *frame);

/* process_exit_self hook: if `proc` is being debugged, post DBG_EV_EXITED. */
void debug_notify_exit(struct process *proc, int code);

/* The syscalls (69-75). Registered in syscall_table. Each takes the register
 * frame and returns an int64_t the dispatcher writes to rax. */
int64_t sys_debug_attach(struct regs *r);
int64_t sys_debug_wait(struct regs *r);
int64_t sys_debug_cont(struct regs *r);
int64_t sys_debug_regs(struct regs *r);
int64_t sys_debug_mem(struct regs *r);
int64_t sys_debug_hwbp(struct regs *r);
int64_t sys_debug_detach(struct regs *r);

#endif /* __PROCESS_DEBUG_H__ */
