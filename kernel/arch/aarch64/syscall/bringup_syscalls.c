#include "include/syscall_abi.h"
#include "include/syscall_nr.h"
#include "include/usercopy.h"
#include "include/errno.h"
#include "include/kprintf.h"
#include "drivers/char/serial.h"
#include "drivers/timer/timer.h"

/* A four-entry syscall table with a deletion date -- docs/ARM64.md phase A5.
 *
 * READ THIS BEFORE ADDING A FIFTH.
 *
 * The real table is kernel/syscall/syscalls.c: 88 handlers that A4 already made
 * architecture-neutral, plus 7 more in process/debug.c. Not one of them needs a
 * line changed to run here. They are not compiled for aarch64 yet for a reason
 * that has nothing to do with this file -- they need process.c, the VFS, the fd
 * layer, the compositor and the IPC code, which is A6's work.
 *
 * What A5 has to prove is the TRANSITION: that `eret` reaches EL0, that `svc`
 * comes back with arguments intact, that the privilege boundary rejects a bad
 * pointer, and that a handler taking `struct sysargs` -- the A4 shape -- can be
 * driven from an aarch64 trap frame. Four syscalls are enough for that and 95
 * would prove nothing extra.
 *
 * So this file defines syscall_invoke() for aarch64 with the same signature
 * kernel/syscall/syscalls.c defines it with for x86. When A6 compiles the real
 * table, THIS FILE IS DELETED and the arch entry point above it does not
 * change by a single line. That is the test of whether A4 was done properly.
 *
 * docs/TODO.md records the deletion. Do not grow this file to avoid doing A6.
 */

extern void el0_exit(int64_t code);      /* usermode.c -- does not return */

/* write(fd, buf, len). Only fd 1 and 2 exist here; there is no fd layer yet.
 *
 * The bounce buffer is the point of the exercise: `buf` is a pointer chosen by
 * a program running at EL0, and the kernel must not dereference it directly no
 * matter how plausible it looks. copy_from_user() validates the whole range
 * against the current address space before a byte moves. */
static int64_t bsys_write(const struct sysargs *a) {
    int         fd  = (int)a->arg[0];
    const void *buf = (const void *)(uintptr_t)a->arg[1];
    size_t      len = (size_t)a->arg[2];

    if (fd != 1 && fd != 2)
        return -EMBK_EINVAL;
    if (len == 0)
        return 0;

    char chunk[256];
    size_t done = 0;

    while (done < len) {
        size_t n = len - done;
        if (n > sizeof chunk)
            n = sizeof chunk;

        if (copy_from_user(chunk, (const char *)buf + done, n) != EMBK_OK) {
            /* Loud, because this is the boundary working. A silent -EFAULT
             * would make a rejected pointer and a rejected fd look identical
             * from the kernel log. */
            kprintf("el0: REFUSED write of %d bytes from %p (not a mapped user address)\n",
                    (int)n, (const void *)((const char *)buf + done));
            return -EMBK_EFAULT;
        }

        for (size_t i = 0; i < n; i++)
            serial_write_char(chunk[i]);
        done += n;
    }
    return (int64_t)done;
}

static int64_t bsys_exit(const struct sysargs *a) {
    el0_exit((int64_t)a->arg[0]);
    return 0;                            /* not reached */
}

/* There is no process table yet, so this is a constant -- and it is here
 * precisely because it takes NO arguments: it proves the syscall number
 * travelled on its own, independent of anything in x0..x5. */
static int64_t bsys_getpid(const struct sysargs *a) {
    (void)a;
    return 1;
}

static int64_t bsys_uptime_ms(const struct sysargs *a) {
    (void)a;
    return (int64_t)timer_uptime_ms();
}

int64_t syscall_invoke(const struct sysargs *a) {
    switch (a->nr) {
    case SYS_write:     return bsys_write(a);
    case SYS_exit:      return bsys_exit(a);
    case SYS_getpid:    return bsys_getpid(a);
    case SYS_uptime_ms: return bsys_uptime_ms(a);
    default:
        kprintf("el0: syscall %d is not implemented on aarch64 yet (A6)\n",
                (int)a->nr);
        return -EMBK_EINVAL;
    }
}
