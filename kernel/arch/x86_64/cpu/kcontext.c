#include "arch/x86_64/cpu/kcontext.h"

/* The parts of context handling that are C rather than assembly. See
 * kcontext.h; kcontext.asm has the switch itself. */

void kernel_ctx_prepare(struct kcontext *ctx, void (*entry)(void),
                        uint64_t kstack_top)
{
    ctx->rbx = ctx->rbp = 0;
    ctx->r12 = ctx->r13 = ctx->r14 = ctx->r15 = 0;
    ctx->rip = (uint64_t)(uintptr_t)entry;

    /* kstack_top - 8, not kstack_top: kernel_ctx_switch (kcontext.asm) enters
     * a brand-new thread's trampoline via a raw `jmp`, not a `call` -- there's
     * no synthetic return address on the stack. kstack_top is page-aligned
     * (0 mod 16, vmm_alloc_kernel_stack), but GCC compiles the trampoline as
     * an ORDINARY C function, which always assumes the standard x86-64 SysV
     * "entered via call" convention: RSP === 8 mod 16 at the function's own
     * first instruction (as if a call just pushed an 8-byte return address).
     * Left as kstack_top verbatim, every aligned-stack local variable anywhere
     * in that thread's initial call chain -- not just in the trampoline
     * itself, arbitrarily deep, e.g. a kthread's own locals -- ends up 8 bytes
     * off from where GCC assumed, and any aligned SSE store/load #GP's. Silent
     * until the FPU/SSE selftest (process_test_fpu) first hit it. Sacrificing
     * 8 bytes off the very top of a stack that never uses them (the
     * trampoline's `ret` never executes -- both trampolines end in a noreturn
     * call + __builtin_unreachable()) costs nothing and makes the real entry
     * RSP match what GCC assumes.
     *
     * aarch64 needs no such fudge: its procedure call standard wants SP 16-byte
     * aligned AT the entry point, with the return address in x30 rather than on
     * the stack. Same requirement, opposite arithmetic -- which is exactly why
     * this line cannot live in the shared scheduler. */
    ctx->rsp = kstack_top - 8;

    /* IF=0 (0x002), NOT 1 (0x202), on purpose: kernel_ctx_switch's popfq
     * restores THIS rflags value into the LIVE flags register before jumping
     * to ctx->rip -- if IF were already 1 here, interrupts would go live a few
     * instructions before the trampoline reaches its own
     * spin_unlock(&g_sched_lock) (its first action), opening a real window
     * where this core's own next timer tick fires, re-enters schedule() on a
     * lock this exact core still holds, and spins on it forever -- observed
     * directly under -smp 4 (docs section 16, Bug 15). spin_unlock() is what
     * turns interrupts back on, at the point that is actually safe. */
    ctx->rflags = 0x002;
}

uint64_t kernel_ctx_pc(const struct kcontext *ctx) { return ctx->rip; }
uint64_t kernel_ctx_fp(const struct kcontext *ctx) { return ctx->rbp; }
