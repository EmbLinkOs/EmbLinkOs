/* syscallbench.c -- the two ways into the kernel, side by side.
 *
 * getpid() is the cheapest syscall there is: it reads one field and returns.
 * Timing a loop of it measures the ENTRY AND EXIT and almost nothing else,
 * which is the whole point -- `int 0x80` walks the IDT, switches stacks
 * through the TSS and pushes a frame; `syscall` drops the selectors from an
 * MSR, saves RIP in RCX and flags in R11, and jumps.
 *
 * Both are issued from here explicitly, in the same process, microseconds
 * apart, so the comparison is not between two runs of anything. Both must
 * also AGREE: a fast path that returns the wrong pid is not fast, it is
 * broken -- checked before either is timed.
 *
 * Exit code: 0 if both paths returned the same pid every time, else 1. */
#include <stdio.h>
#include <stdint.h>
#include "embk.h"

#define N 200000

#if !defined(__x86_64__)
/* The two entries are an x86 question: aarch64 has one instruction for the
 * trap (`svc #0`) and no second path to compare it with. Built on both
 * because every program directory is built on both; says so and exits clean
 * on the other. */
int main(void) {
    printf("syscallbench: x86 only -- aarch64 enters the kernel one way, with `svc`\n");
    return 0;
}
#else

static inline int64_t getpid_int80(void) {
    int64_t r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"((int64_t)EMBK_SYS_getpid) : "rcx", "r11", "memory");
    return r;
}
static inline int64_t getpid_syscall(void) {
    int64_t r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"((int64_t)EMBK_SYS_getpid) : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    int64_t a = getpid_int80(), b = getpid_syscall();
    printf("syscallbench: int 0x80 says pid %lld, syscall says pid %lld\n",
           (long long)a, (long long)b);
    if (a != b || a <= 0) {
        printf("syscallbench: the two paths disagree -- FAIL\n");
        return 1;
    }

    /* Warm both paths before either is timed. */
    for (int i = 0; i < 1000; i++) { (void)getpid_int80(); (void)getpid_syscall(); }

    int bad = 0;
    uint64_t t0 = embk_uptime_ms();
    for (int i = 0; i < N; i++) if (getpid_int80() != a) bad++;
    uint64_t t1 = embk_uptime_ms();
    for (int i = 0; i < N; i++) if (getpid_syscall() != a) bad++;
    uint64_t t2 = embk_uptime_ms();

    uint64_t i80 = (t1 - t0), fast = (t2 - t1);
    printf("syscallbench: %d getpid each -- int 0x80 %llu ms (%llu ns/call), "
           "syscall %llu ms (%llu ns/call)\n",
           N, (unsigned long long)i80, (unsigned long long)(i80 * 1000000ull / N),
           (unsigned long long)fast, (unsigned long long)(fast * 1000000ull / N));
    if (fast && i80)
        printf("syscallbench: the fast path is %llu%% of the cost\n",
               (unsigned long long)(fast * 100 / i80));
    if (bad) printf("syscallbench: %d call(s) returned the wrong pid -- FAIL\n", bad);
    return bad ? 1 : 0;
}
#endif /* __x86_64__ */
