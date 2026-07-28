/* user/bin/init.c -- EmbLink OS init: the first user process, the ROOT of
 * userspace authority.
 *
 * The kernel spawns exactly ONE user process at boot -- this one (see
 * kernel/main.c). init holds the full namespace + all capabilities; every other
 * process's authority is a NARROWING of init's (docs/USERSPACE_v2.md -- "authority
 * IS the namespace"). This is the spine the old design was missing: until now the
 * kernel spawned the desktop (home.elf) DIRECTLY, so there was no root of
 * authority and no supervisor -- a died desktop was a dead session with nobody to
 * notice. Now the desktop is init's CHILD.
 *
 * (Numerically this is not pid 1 -- the per-core idle kthreads take the low pids
 * -- but it is the first *user* process and the root of the user authority tree;
 * "pid 1" throughout the docs names that role, not the integer.)
 *
 * UP1 scope is deliberately small (the per-process namespace mechanism lands in
 * UP2, docs/USERSPACE_v2.md):
 *   1. Bring up the session desktop (/system/bin/home.elf).
 *   2. Supervise it -- if it exits or crashes, reap the zombie and respawn, so
 *      the machine always has a desktop.
 *   3. Never exit: init staying alive is what keeps userspace alive.
 *
 * Orphaned grandchildren (apps left behind if the desktop dies) are reaped by
 * the KERNEL (process.c: a process with no live parent is freed directly), so
 * init only needs to watch its own direct child.
 *
 * Freestanding: own _start, no libc -- init carries no dependency it would then
 * have to keep alive. Built with user/lib/user.ld, like primtest.elf.
 */

#include "embk.h"

#define DESKTOP "/system/bin/home.elf"

static void log_line(const char *s) { embk_puts(1, s); }

/* Append a signed decimal to a small buffer (no libc here). Returns end ptr. */
static char *put_dec(char *p, long v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

void _start(long argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    log_line("init: up -- root of EmbLink userspace authority\n");

    for (;;) {
        char *dargv[] = { (char *)DESKTOP, NULL };
        int h = (int)embk_spawn(DESKTOP, dargv, NULL, 0);
        if (h < 0) {
            log_line("init: could not spawn the desktop; retrying in 1s\n");
            embk_sleep_ms(1000);
            continue;
        }
        log_line("init: desktop session started (" DESKTOP ")\n");

        /* Block until the desktop exits, then reap it. The wait frees BOTH the
         * zombie process slot and this spawn handle -- without it, every restart
         * would leak one of init's 16 handles. */
        int code = embk_wait(h);

        char b[96], *p = b;
        const char *pre = "init: desktop exited (code ";
        while (*pre) *p++ = *pre++;
        p = put_dec(p, code);
        const char *post = ") -- restarting session\n";
        while (*post) *p++ = *post++;
        *p = 0;
        log_line(b);

        embk_sleep_ms(200);   /* never a hot crash-loop */
    }
}
