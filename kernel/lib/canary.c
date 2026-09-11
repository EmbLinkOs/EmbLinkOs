#include <stdint.h>
#include "include/types.h"
#include "include/kprintf.h"
#include "include/arch_thread.h"   /* kernel_ctx_save / kernel_ctx_restore */
#include "lib/canary.h"
#include "include/arch_irq.h"      /* arch_cpu_idle: the halt */

/* ==========================================================================
 * THE KERNEL STACK CANARY.
 *
 * -fstack-protector-strong makes the compiler put a copy of this word between
 * a function's locals and its return address, and check it before returning.
 * A buffer overflow that runs off the end of a local array into the saved
 * return address has to walk through the canary to get there, and when it
 * does, the function never returns: __stack_chk_fail runs instead.
 *
 * ONE GLOBAL WORD, and why not per-thread. GCC's x86-64 default keeps the
 * canary at %fs:0x28, but in this kernel FS_BASE is the USER thread pointer
 * (arch/x86_64/cpu/fsbase.h) -- a kernel read of %fs:0x28 would read the
 * current process's TLS, which is user memory (SMAP faults it) and which the
 * process controls. So both architectures use -mstack-protector-guard=global,
 * and the guard is this variable. Per-CPU canaries via GS_BASE are a later
 * refinement; a single random word already defeats the overflow that does
 * not know it.
 *
 * SEEDED IN ASSEMBLY, BEFORE THE FIRST C FRAME. A protected function reads the
 * guard on entry and compares on exit, so the guard must never change while
 * any protected frame is live -- and kernel_main's frame is live for the
 * whole boot. It is therefore written by the entry stub (kentry / boot.S)
 * from the cycle counter, once, before any C runs, and NEVER rewritten: not
 * by random_init(), whose better entropy arrives too late to be usable here.
 * The initialiser below is what the guard is if the stub somehow did not run;
 * it is non-zero so a missed seed is a weak canary rather than no canary.
 *
 * WHY THE TSC IS ENOUGH FOR THIS WORD. The canary does not need to be
 * unpredictable to an attacker who can already read kernel memory -- that
 * attacker has lost the game a different way. It needs to be unknown to a
 * WRITE that cannot read, which is what a linear overflow is. A boot-time
 * cycle count the attacker cannot observe satisfies that.
 * ========================================================================== */
uintptr_t __stack_chk_guard = 0x5A6B7C8D9EAFB0C1ULL;

/* --- the test hook ---------------------------------------------------------
 * A canary that is never seen to fire is a claim, not a protection. `test
 * canary` overflows a local on purpose; the failure handler would panic, so
 * the test arms a recovery point first and the handler jumps to it instead.
 * The same setjmp/longjmp the usercopy guard uses (include/uaccess_guard.h),
 * for the same reason: it exists on both architectures already. */
volatile bool     g_canary_test_armed;
struct kcontext   g_canary_test_ctx;
static volatile uint64_t g_canary_fires;

void canary_test_disarm(void) { g_canary_test_armed = false; }
uint64_t canary_fires(void)   { return g_canary_fires; }
uintptr_t canary_value(void)  { return __stack_chk_guard; }

__attribute__((noreturn)) void __stack_chk_fail(void) {
    g_canary_fires++;
    if (g_canary_test_armed) {
        g_canary_test_armed = false;
        kernel_ctx_restore(&g_canary_test_ctx, 1);   /* does not return */
    }
    /* A real smash. The frame that overflowed is the CALLER of this function,
     * and its return address is exactly what was overwritten -- so
     * __builtin_return_address(0) is the corrupted value, not a location. The
     * caller's identity has to come from the stack pointer instead, which the
     * panic dump prints. Loud, and then stop: a kernel whose return address
     * has been rewritten must not take the return. */
    kprintf("\n*** KERNEL STACK SMASHING DETECTED *** canary 0x%lx failed; the "
            "function that returned here had its frame overwritten. Halting.\n",
            (unsigned long)__stack_chk_guard);
    for (;;) { arch_cpu_idle(); }
}

/* The victim for `test canary`. NOINLINE so it has a frame and a canary of its
 * own; the length arrives through a volatile so the compiler cannot see the
 * overflow and either warn it away or delete it. The writes go PAST the
 * buffer -- into the canary and then the saved return address -- which is the
 * shape of every stack-smashing bug this exists to stop. */
__attribute__((noinline, optimize("no-tree-loop-optimize")))
void canary_smash_a_frame(int len) {
    volatile int n = len;
    char buf[16];
    for (int i = 0; i < n; i++)
        ((volatile char *)buf)[i] = (char)(0x41 + i);
    __asm__ volatile("" :: "r"(buf) : "memory");   /* buf must exist */
}
