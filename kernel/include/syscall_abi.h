#ifndef _SYSCALL_ABI_H_
#define _SYSCALL_ABI_H_

#include <stdint.h>

/* The syscall calling convention, expressed once, without a machine in it.
 * docs/ARM64.md §2.4, phase A4.
 *
 * WHAT THIS REPLACED, AND WHY IT MATTERED. Every syscall handler used to take
 * `struct regs *` and read its arguments as `r->rdi`, `r->rsi`, `r->rdx`,
 * `r->r10`, `r->r8`, `r->r9` -- 199 such reads across 95 handlers (184 in 88
 * handlers under arch/x86_64/syscall/, plus 15 more in 7 handlers that had
 * quietly grown in kernel/process/debug.c, outside the file anyone counted).
 * Their POLICY
 * was already portable: sys_open parses a path and asks the VFS, sys_win_move
 * moves a window, and neither of those is an x86 idea. They were welded to one
 * architecture purely by how they collected their arguments, and that is why
 * the whole file lived under arch/x86_64/ despite almost none of it being
 * architecture-specific.
 *
 * The arch entry point now extracts ONCE, into this struct, and the handlers
 * stop knowing where arguments come from:
 *
 *     x86_64   fills arg[0..5] from rdi, rsi, rdx, r10, r8, r9
 *     aarch64  will fill them from x0..x5
 *
 * Six arguments, because six is what both calling conventions pass in
 * registers before spilling to the stack, and a syscall that needs a seventh
 * should take a pointer to a struct instead -- as sys_spawn already does.
 *
 * WHY THERE IS NO POINTER TO THE TRAP FRAME HERE. It was checked: not one of
 * the 89 handlers reads anything from `struct regs` other than an argument
 * register. Adding an escape hatch nobody needs would guarantee somebody
 * eventually uses it, and one handler reaching into the frame puts the whole
 * file back under arch/. If a handler ever genuinely needs the interrupted
 * context -- a debugger reading the FAULTING thread's registers is the
 * plausible case, and process/debug.c already does that a different way -- it
 * should ask for it through a named arch call, not by being handed the raw
 * frame. */

#define SYSCALL_MAX_ARGS 6

struct sysargs {
    uint64_t nr;                        /* which syscall                     */
    uint64_t arg[SYSCALL_MAX_ARGS];     /* its arguments, machine-independent */
};

/* Every handler's signature. `const` is load-bearing: a handler that could
 * write back into the frame is a handler that has an architecture again. */
typedef int64_t (*syscall_handler_t)(const struct sysargs *);

/* Look up and run one syscall. Returns the value to hand back to user space,
 * or -EMBK_EINVAL for an unknown number. Called by each architecture's trap
 * entry after it has filled in `a`. */
int64_t syscall_invoke(const struct sysargs *a);

/* --- the one genuinely per-architecture thing a handler still needs --------
 *
 * Install the calling thread's TLS base -- what a compiler-generated
 * thread-local access reads through. x86_64 writes the IA32_FS_BASE MSR;
 * aarch64 will write TPIDR_EL0. Same concept, no shared spelling, so it is a
 * named call rather than an #ifdef.
 *
 * It lives in this header rather than a HAL header because there is exactly
 * one of these today and inventing an arch_* header for one function would be
 * pre-abstracting against a single implementation (docs/ARM64.md §2.3). When
 * A5 factors the real seam, this moves with it. */
void arch_tls_base_set(uint64_t base);

#endif /* _SYSCALL_ABI_H_ */
