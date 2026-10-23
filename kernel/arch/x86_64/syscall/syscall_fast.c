/* kernel/arch/x86_64/syscall/syscall_fast.c -- `syscall`/`sysret`.
 *
 * The stub is in syscall_entry.asm (syscall_fast_entry) and explains the
 * mechanism, including why this needs no swapgs. This file is the two ends of
 * it: the MSRs that arm the instruction on each core, and the C the stub
 * calls.
 *
 * `int 0x80` stays installed. It is the fallback for anything already
 * compiled against it (prebuilt binaries in the image), and `test syscall`
 * times both so the difference is a number rather than a claim. */
#include <stdint.h>
#include "arch/x86_64/syscall/syscall.h"
#include "include/syscall_abi.h"     /* struct sysargs, syscall_invoke */
#include "include/types.h"
#include "arch/x86_64/cpu/percpu.h"
#include "process/process.h"
#include "include/kprintf.h"

#define MSR_EFER        0xC0000080u
#define MSR_STAR        0xC0000081u
#define MSR_LSTAR       0xC0000082u
#define MSR_FMASK       0xC0000084u
#define MSR_GS_BASE     0xC0000101u

#define EFER_SCE        (1ull << 0)

/* The GDT is already in SYSRET's order, which is not an accident of ours --
 * it is the order SYSRET demands: kernel code, kernel data, USER DATA, user
 * code. SYSCALL loads CS = STAR[47:32] and SS = that + 8; SYSRET loads
 * CS = STAR[63:48] + 16 and SS = that + 8, both forced to RPL 3. With kernel
 * code 0x08, kernel data 0x10, user data 0x18 and user code 0x20:
 *     STAR[47:32] = 0x08  ->  kernel CS 0x08, kernel SS 0x10
 *     STAR[63:48] = 0x10  ->  user   CS 0x23, user   SS 0x1B
 * which is exactly what the iretq path uses, so a thread cannot tell which
 * way it entered or left. */
#define STAR_SYSCALL_SEL  0x08ull
#define STAR_SYSRET_SEL   0x10ull

/* Flags the CPU clears on entry. IF above all -- a syscall must begin with
 * interrupts off, exactly as the int-0x80 interrupt gate begins -- then the
 * ones that would let a caller's leftover state steer the kernel: TF (a
 * single-step that would trap inside the kernel), DF (string ops running
 * backwards), AC (alignment checks, and with SMAP the AC bit is the thing
 * that says "kernel may touch user memory" -- entering with it set would
 * disable the protection for the whole syscall), NT and IOPL. */
#define SYSCALL_FMASK  ((1ull<<8)|(1ull<<9)|(1ull<<10)|(1ull<<14)|(3ull<<12)|(1ull<<18))

static inline uint64_t rd(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Does this processor have SYSCALL at all? Every x86-64 does (it is how
 * 64-bit code has entered the kernel since AMD defined the mode), but the
 * bit is architectural and free to read, and a kernel that assumes a feature
 * it never checked is a kernel that triple-faults on the machine that
 * lacks it. */
static bool syscall_supported(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
    (void)a; (void)b; (void)c;
    return (d & (1u << 11)) != 0;      /* EDX.SYSCALL/SYSRET */
}

static bool g_fast_on;
bool syscall_fast_enabled(void) { return g_fast_on; }

void syscall_fast_init_this_cpu(void) {
    extern void syscall_fast_entry(void);
    struct cpu_data *me = this_cpu();

    /* The scratch this core's stub reaches through GS: gs:0 parks the user
     * RSP for the length of the entry, gs:8 is the kernel stack to switch to
     * -- kept equal to TSS.RSP0, which the scheduler already updates on every
     * switch (tss_set_rsp0). GS.base is written AFTER the GDT load, because
     * loading a GS selector reloads the base from the descriptor (zero) and
     * would undo this. Nothing reloads GS afterwards. */
    me->sc.user_rsp   = 0;
    me->sc.kstack_top = me->tss.rsp0;
    wr(MSR_GS_BASE, (uint64_t)&me->sc);

    if (!syscall_supported()) {
        if (me->cpu_index == 0)
            kprintf("syscall: this processor has no SYSCALL/SYSRET -- int 0x80 only\n");
        return;
    }

    wr(MSR_STAR,  (STAR_SYSRET_SEL << 48) | (STAR_SYSCALL_SEL << 32));
    wr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_fast_entry);
    wr(MSR_FMASK, SYSCALL_FMASK);
    wr(MSR_EFER,  rd(MSR_EFER) | EFER_SCE);

    g_fast_on = true;
    if (me->cpu_index == 0)
        kprintf("syscall: SYSCALL/SYSRET armed (entry %p, int 0x80 still accepted)\n",
                (void *)(uintptr_t)syscall_fast_entry);
}

/* What the stub builds on the kernel stack. The order is the stub's push
 * order reversed -- see syscall_entry.asm, and change neither alone. */
struct syscall_fast_frame {
    uint64_t r11, rcx;                 /* the user's RFLAGS and RIP */
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t rax;                      /* number in, result out */
    uint64_t user_rsp;                 /* the stack to return on -- per THREAD */
};

/* The same contract sys_dispatch has for int 0x80: enable interrupts (a
 * syscall that waits on a disk must be able to take the completion), count
 * the kernel path so a kill waits for it, run the call, and die at the exit
 * if one arrived meanwhile. */
void syscall_fast_dispatch(struct syscall_fast_frame *f) {
    __asm__ volatile ("sti");

    struct sysargs a;
    a.nr     = f->rax;
    a.arg[0] = f->rdi;
    a.arg[1] = f->rsi;
    a.arg[2] = f->rdx;
    a.arg[3] = f->r10;
    a.arg[4] = f->r8;
    a.arg[5] = f->r9;

    if (current_thread) current_thread->in_kernel++;
    f->rax = (uint64_t)syscall_invoke(&a);
    if (current_thread) {
        current_thread->in_kernel--;
        if (current_thread->killed)
            thread_die_killed();        /* never returns */
    }
}
