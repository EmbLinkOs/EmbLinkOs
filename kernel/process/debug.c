/* Live-debugging kernel contract (EMBDBG_Specification.md §6), M1.
 *
 * The kernel provides PRIMITIVES; the debugger composes POLICY. A software
 * breakpoint is the debugger planting 0xCC via sys_debug_mem and keeping the
 * byte — the kernel holds no breakpoint table. Its jobs:
 *   - deliver a fault/#BP/#DB from a debugged thread as a STOP EVENT, not a
 *     panic (debug_on_exception, called from isr_handler §6.6);
 *   - let the debugger read/write a stopped thread's registers (sys_debug_regs)
 *     and the target's memory across address spaces (sys_debug_mem);
 *   - resume, optionally single-stepping via the trap flag (sys_debug_cont);
 *   - block the debugger until the target stops (sys_debug_wait), in the
 *     ordinary blocking-syscall style — a debugger is just a process that
 *     blocks in a syscall, like a reader in read().
 *
 * The debug HANDLE is the child process handle the debugger already holds from
 * spawn (SPAWN_ACTION_DEBUG attaches the session to that child); a distinct
 * typed handle is a later refinement. Everything is gated on EMBK_CAP_DEBUG. */

#include "process/debug.h"
#include "process/process.h"
#include "arch/x86_64/syscall/syscall.h"
#include "arch/x86_64/syscall/usercopy.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "include/errno.h"
#include "include/kstring.h"
#include "include/kprintf.h"

/* A tiny static pool — a handful of concurrent debug sessions is plenty, and
 * it sidesteps allocator lifetime races on the fault path. */
#define DEBUG_SESSION_MAX 8
static struct debug_session g_sessions[DEBUG_SESSION_MAX];

#define TF_BIT 0x100ULL   /* RFLAGS.TF — one-shot single step */

static struct debug_session *session_alloc(void) {
    for (int i = 0; i < DEBUG_SESSION_MAX; i++)
        if (!g_sessions[i].used) {
            struct debug_session *s = &g_sessions[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
            return s;
        }
    return NULL;
}

struct debug_session *debug_session_spawn(struct process *debugger,
                                          struct process *target,
                                          struct thread *target_thread) {
    struct debug_session *s = session_alloc();
    if (!s) return NULL;
    s->target = target;
    s->debugger = debugger;
    s->stopped_thread = target_thread;
    s->stopped_frame = NULL;                 /* no frame until it runs & faults */
    /* Born stopped BEFORE _start: park the never-run thread on target_wq, and
     * record an initial stop the debugger's first wait can observe. */
    sched_lock();
    wait_queue_block(&s->target_wq, target_thread);
    s->stopped = 1;
    s->event.tid = target->pid;
    s->event.reason = DBG_EV_STEP;           /* "stopped at entry" */
    s->event.pc = target_thread->entry_point;
    sched_unlock();
    return s;
}

/* isr_handler hook: a fault from a debugged thread. Park it, wake the debugger,
 * and block until sys_debug_cont resumes us. Returns 1 (caller must iretq). */
int debug_on_exception(struct thread *t, struct regs *frame) {
    struct debug_session *s = t->proc->debug_session;
    if (!s) return 0;
    uint64_t vec = frame->int_no;            /* == struct registers.vector */

    sched_lock();
    s->stopped = 1;
    s->stopped_thread = t;
    s->stopped_frame = frame;                /* lives on t's kstack while parked */
    s->event.tid = t->proc->pid;
    s->event.vector = (uint32_t)vec;
    s->event.error_code = (uint32_t)frame->err_code;
    s->event.fault_addr = 0;
    if (vec == 3) {
        /* #BP: the CPU left RIP just past the 0xCC. Report the breakpoint's own
         * address (RIP-1); the debugger restores the byte and rewinds RIP. */
        s->event.reason = DBG_EV_BREAKPOINT;
        s->event.pc = frame->rip - 1;
    } else if (vec == 1) {
        s->event.reason = DBG_EV_STEP;
        s->event.pc = frame->rip;
    } else {
        s->event.reason = DBG_EV_FAULT;      /* crashed into the debugger */
        s->event.pc = frame->rip;
        if (vec == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            s->event.fault_addr = cr2;
        }
    }
    wait_queue_wake_one(&s->debugger_wq);
    /* Blocks the CURRENT (faulting) thread; returns with the lock released once
     * sys_debug_cont wakes target_wq. The frame may have been edited (regs, TF)
     * in the meantime — the caller's iretq honours it. */
    sched_block_current_locked(&s->target_wq);
    return 1;
}

void debug_notify_exit(struct process *proc, int code) {
    struct debug_session *s = proc->debug_session;
    if (!s) return;
    sched_lock();
    s->exited = 1;
    s->stopped = 1;
    s->stopped_frame = NULL;
    s->event.tid = proc->pid;
    s->event.reason = DBG_EV_EXITED;
    s->event.error_code = (uint32_t)code;
    s->event.pc = 0;
    wait_queue_wake_one(&s->debugger_wq);
    sched_unlock();
}

/* Resolve the debug handle (== the child proc handle) to its session, checking
 * the caller owns it and holds CAP_DEBUG. */
static struct debug_session *dbg_resolve(int handle) {
    if (!(process_current_caps() & EMBK_CAP_BIT(EMBK_CAP_DEBUG))) return NULL;
    uint32_t pid;
    if (process_handle_resolve(current_process, handle, &pid) != 0) return NULL;
    struct process *p = process_find(pid);
    if (!p || !p->debug_session) return NULL;
    if (p->debug_session->debugger != current_process) return NULL;
    return p->debug_session;
}

int64_t sys_debug_wait(struct regs *r) {
    struct debug_session *s = dbg_resolve((int)r->rdi);
    if (!s) return -EMBK_EINVAL;
    void *uev = (void *)r->rsi;

    sched_lock();
    while (!(s->stopped || s->exited)) {
        sched_block_current_locked(&s->debugger_wq);   /* returns lock released */
        sched_lock();
    }
    struct embk_debug_event ev = s->event;
    sched_unlock();

    if (uev && copy_to_user(uev, &ev, sizeof ev) != EMBK_OK) return -EMBK_EFAULT;
    return (int64_t)ev.tid;
}

int64_t sys_debug_cont(struct regs *r) {
    struct debug_session *s = dbg_resolve((int)r->rdi);
    if (!s) return -EMBK_EINVAL;
    int action = (int)r->rdx;                /* (dbg, tid, action, data) */
    if (s->exited) return -EMBK_EINVAL;

    sched_lock();
    if (!s->stopped) { sched_unlock(); return -EMBK_EINVAL; }
    if (s->stopped_frame) {
        if (action == DBG_STEP) s->stopped_frame->eflags |= TF_BIT;
        else                    s->stopped_frame->eflags &= ~TF_BIT;
    }
    s->stopped = 0;
    wait_queue_wake_one(&s->target_wq);      /* resume born-stopped or parked thread */
    sched_unlock();
    return 0;
}

int64_t sys_debug_regs(struct regs *r) {
    struct debug_session *s = dbg_resolve((int)r->rdi);
    if (!s) return -EMBK_EINVAL;
    void *buf = (void *)r->rdx;               /* (dbg, tid, buf, len, write) */
    uint64_t len = r->r10;
    int write = (int)r->r8;
    if (!s->stopped || !s->stopped_frame) return -EMBK_EINVAL;  /* no live frame */
    if (len > sizeof(struct regs)) len = sizeof(struct regs);
    if (write) {
        if (copy_from_user(s->stopped_frame, buf, len) != EMBK_OK) return -EMBK_EFAULT;
    } else {
        if (copy_to_user(buf, s->stopped_frame, len) != EMBK_OK) return -EMBK_EFAULT;
    }
    return (int64_t)len;
}

/* Cross-address-space memory: walk the TARGET's pml4 a page at a time, reach
 * the physical frame through the direct map, and copy the overlapping range to
 * or from the debugger's buffer. This is what plants breakpoints and reads
 * variables (§6.4). Writes hit the physical frame directly, bypassing the
 * target's page protections — exactly a debugger's job. */
int64_t sys_debug_mem(struct regs *r) {
    struct debug_session *s = dbg_resolve((int)r->rdi);
    if (!s) return -EMBK_EINVAL;
    uint64_t addr = r->rsi;                   /* (dbg, addr, buf, len, write) */
    uint8_t *ubuf = (uint8_t *)r->rdx;
    uint64_t len = r->r10;
    int write = (int)r->r8;
    struct process *tgt = s->target;
    if (!tgt) return -EMBK_EINVAL;

    uint64_t done = 0;
    while (done < len) {
        uint64_t va = addr + done;
        uint64_t page = va & ~0xFFFULL;
        uint64_t phys = vmm_get_phys_in(tgt->pml4_phys, page);
        if (!phys) break;                     /* unmapped in the target */
        uint64_t off = va & 0xFFF;
        uint64_t n = 0x1000 - off;
        if (n > len - done) n = len - done;
        uint8_t *kpage = (uint8_t *)(uintptr_t)P2V(phys) + off;
        if (write) {
            if (copy_from_user(kpage, ubuf + done, n) != EMBK_OK) break;
        } else {
            if (copy_to_user(ubuf + done, kpage, n) != EMBK_OK) break;
        }
        done += n;
    }
    return (int64_t)done;
}

int64_t sys_debug_detach(struct regs *r) {
    struct debug_session *s = dbg_resolve((int)r->rdi);
    if (!s) return -EMBK_EINVAL;
    int disp = (int)r->rsi;
    struct process *tgt = s->target;

    sched_lock();
    if (tgt) tgt->debug_session = NULL;       /* stop routing its faults here */
    int was_stopped = s->stopped;
    s->stopped = 0;
    if (disp != DETACH_KILL && was_stopped && !s->exited)
        wait_queue_wake_one(&s->target_wq);   /* let it run */
    sched_unlock();

    if (disp == DETACH_KILL && tgt) process_kill(tgt->pid);
    s->used = 0;                              /* target won't touch s after wake */
    return 0;
}

/* M1 stubs: attach-to-running and hardware watchpoints are refinements the
 * born-under-debug + software-breakpoint path does not need. Named, not faked. */
int64_t sys_debug_attach(struct regs *r) { (void)r; return -EMBK_ENOSYS; }
int64_t sys_debug_hwbp(struct regs *r)   { (void)r; return -EMBK_ENOSYS; }
