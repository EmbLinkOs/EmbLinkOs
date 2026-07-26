#ifndef __DEBUG_ABI_H__
#define __DEBUG_ABI_H__

/* EmbDBG kernel debugging contract — the ABI shared by the kernel and any
 * userspace debugger (EMBDBG_Specification.md §6). Syscall numbers 69-75 and
 * SPAWN_ACTION_DEBUG (file-action id 6) are the surface; this header is the
 * data that crosses the boundary.
 *
 * THE MODEL (§6.3): software breakpoints are debugger-side. The debugger plants
 * 0xCC with sys_debug_mem and keeps the saved byte itself. The kernel's only
 * job is to deliver #BP/#DB/faults as STOP EVENTS instead of a panic, to give
 * register/memory access to a stopped thread, and to single-step on resume.
 * The kernel holds no breakpoint table. */

#include <stdint.h>

/* Syscall numbers — must match the SYS_debug_* entries in syscall.c and the
 * EMBK_SYS_debug_* mirror in user/lib/embk_syscall.h. */
#define SYS_debug_attach 69
#define SYS_debug_wait   70
#define SYS_debug_cont   71
#define SYS_debug_regs   72
#define SYS_debug_mem    73
#define SYS_debug_hwbp   74
#define SYS_debug_detach 75

/* Spawn file-action: born under debug, stopped before _start (§6.2). */
#define SPAWN_ACTION_DEBUG 6

/* The stop event sys_debug_wait fills (§6.5). */
struct embk_debug_event {
    uint32_t tid;          /* which thread stopped */
    uint32_t reason;       /* DBG_EV_* */
    uint64_t pc;           /* RIP at the stop */
    uint64_t fault_addr;   /* CR2 for a page fault, else 0 */
    uint32_t vector;       /* CPU exception vector, or 0 */
    uint32_t error_code;   /* CPU error code, or exit status for EXITED */
    uint64_t hwbp_hit;     /* bitmask of DRx that fired, else 0 */
};

/* event reasons */
#define DBG_EV_BREAKPOINT    1   /* #BP (planted 0xCC) */
#define DBG_EV_STEP          2   /* #DB from the trap flag */
#define DBG_EV_WATCHPOINT    3   /* #DB from a DRx hardware breakpoint */
#define DBG_EV_FAULT         4   /* any other exception — the program crashed */
#define DBG_EV_THREAD_CREATE 5
#define DBG_EV_THREAD_EXIT   6
#define DBG_EV_EXITED        7   /* the target process exited (error_code=status) */

/* sys_debug_cont actions */
#define DBG_CONT 0   /* run */
#define DBG_STEP 1   /* set TF, execute one instruction, stop with DBG_EV_STEP */

/* sys_debug_detach dispositions */
#define DETACH_RUN  0   /* remove injected state, let it run */
#define DETACH_KILL 1   /* remove injected state, kill the target */

/* sys_debug_hwbp kinds */
#define HW_EXEC  0
#define HW_WRITE 1
#define HW_RDWR  2

/* sys_debug_regs buffer: the saved GP-register frame of a stopped thread.
 * Layout IS struct regs (kernel/arch/x86_64/syscall/syscall.h) so a debugger
 * and the kernel agree field-for-field. */

#endif /* __DEBUG_ABI_H__ */
