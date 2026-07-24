/* crasher.c -- deliberately faults from ring 3, to prove the kernel SURVIVES a
 * userspace crash instead of halting the whole machine.
 *
 * A userspace bug (here, a write through a null pointer) is the PROCESS's fault,
 * not the kernel's. isr_handler's ring-3 arm (EMBDBG_Specification.md §6.6)
 * turns the resulting #PF into process_exit_self(PROCESS_EXIT_FAULT(vector)), so
 * only this process dies -- the parent's process_wait() returns, and the kernel
 * keeps scheduling. `test faultkill` spawns this, waits, checks the fault exit
 * code, and -- the real proof -- keeps running to check at all, twice, so the
 * panic_lock release on the recoverable path is exercised too.
 *
 * Optional arg selects the fault so both a #PF and a #DE are reachable:
 *   (none) | "null"  -> write through NULL         -> #PF, vector 14
 *   "div"            -> divide by zero              -> #DE, vector 0
 * exit:  never returns normally; if it somehow does, exit 77 so the test notices
 *        the fault did NOT fire.
 */
#include <string.h>

int main(int argc, char **argv)
{
    const char *how = (argc > 1) ? argv[1] : "null";

    if (strcmp(how, "div") == 0) {
        volatile int z = 0;
        volatile int x = 1 / z;      /* #DE (vector 0) */
        return x;                    /* unreachable */
    }

    volatile int *p = (volatile int *)0;
    *p = 0xDEAD;                     /* #PF (vector 14) -- NULL is unmapped in ring 3 */

    return 77;                       /* unreachable if the fault fired */
}
