#include "arch/aarch64/sched/bringup.h"
#include "arch/aarch64/cpu/kcontext.h"
#include "mm/pmm.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/arch_irq.h"

/* See bringup.h. This file has a scheduled deletion date. */

#define MAX_THREADS   8
#define STACK_PAGES   2      /* 8 KiB: kprintf's formatter is the deepest
                              * thing a bring-up thread calls */

struct kthread {
    struct kcontext ctx;
    void          (*entry)(void);   /* run by bringup_entry(), below */
    uint64_t        stack_phys;
    uint64_t        slices;
    const char     *name;
    bool            used;
    bool            done;
};

/* The offsets in kcontext.S are hand-written. If the struct grows a field or
 * changes order, every saved register lands in the wrong place and the failure
 * is a jump to a garbage address with no clue as to why. */
_Static_assert(sizeof(struct kcontext) == 112, "kcontext.S offsets assume 112 bytes");
_Static_assert(__builtin_offsetof(struct kcontext, sp)   == 88,  "kcontext.S K_SP");
_Static_assert(__builtin_offsetof(struct kcontext, pc)   == 96,  "kcontext.S K_PC");
_Static_assert(__builtin_offsetof(struct kcontext, daif) == 104, "kcontext.S K_DAIF");

static struct kthread threads[MAX_THREADS];
static int current = -1;
static bool running;

void kernel_ctx_prepare(struct kcontext *ctx, void (*entry)(void),
                        uint64_t kstack_top) {
    extern void kthread_trampoline(void);

    memset(ctx, 0, sizeof(*ctx));

    /* x19 is callee-saved, so restoring the context delivers the entry point
     * to the trampoline for free -- no stack frame has to be forged, which is
     * the part of thread creation that is normally fiddly and
     * machine-specific. */
    ctx->x19 = (uint64_t)(uintptr_t)entry;
    ctx->pc  = (uint64_t)(uintptr_t)kthread_trampoline;

    /* SP 16-byte aligned AT the entry point -- the AArch64 procedure call
     * standard's requirement, and an unaligned SP raises an SP alignment fault
     * rather than misbehaving quietly. Note x86 needs `kstack_top - 8` for the
     * same goal, because there the return address lives on the stack. */
    ctx->sp = kstack_top & ~0xFULL;

    /* DAIF: EVERYTHING MASKED. The thread's entry code turns interrupts on,
     * and it must be the one to do it, because of what it does FIRST.
     *
     * A new thread is entered from inside schedule_locked(), which holds
     * g_sched_lock across the switch; the lock is released by the thread's
     * own first action (process.c: kthread_trampoline / process_trampoline).
     * Until then this core holds the scheduler lock. This was DAIF = 0 --
     * interrupts on from the first instruction -- and so a timer interrupt
     * already pending at the switch was taken in the few instructions
     * between the `msr daif` in CTX_LOAD and that release: its handler
     * called schedule(), which spun forever on the lock this same core
     * held, and the other three cores piled up behind it. A rare, total
     * hang, found by the Raspberry Pi's boot (docs/RPI4.md P3): all four
     * cores in spin_lock, g_sched_lock's holder_lr naming schedule(), and
     * two backtraces ending at the trampoline's frame-chain reset.
     *
     * What DAIF = 0 was protecting against is still true -- a thread that
     * inherited the IRQ handler's masked state would never be preempted --
     * and it is answered where x86 answers it (x86 fabricates IF=0 too): by
     * the entry code enabling interrupts explicitly once the lock is gone.
     * The scheduler's trampolines do (kthread_trampoline's arch_irq_enable;
     * process_trampoline's eret to EL0); the bring-up threads below do in
     * bringup_entry(). */
    ctx->daif = 0x3c0;
}

/* The entry of every bring-up thread. This scheduler holds no lock across a
 * switch, so there is nothing to release -- only the interrupts that
 * kernel_ctx_prepare() now leaves masked to turn on, or the thread is never
 * preempted. */
static void bringup_entry(void) {
    arch_irq_enable();
    threads[current].entry();
}

void bringup_sched_init(void) {
    memset(threads, 0, sizeof(threads));

    /* Thread 0 IS the context we are running in. Its kcontext is filled the
     * first time something switches away from it, exactly like every other
     * thread; nothing special has to be constructed. */
    threads[0].used = true;
    threads[0].name = "boot";
    current = 0;
    running = true;

    kprintf("sched: bring-up round-robin active, this context is thread 0\n");
}

int bringup_thread_create(const char *name, void (*entry)(void)) {
    for (int i = 1; i < MAX_THREADS; i++) {
        if (threads[i].used)
            continue;

        /* Contiguous pages, because a stack that is not contiguous is not a
         * stack. pmm has no multi-page allocator, so this asks for STACK_PAGES
         * and verifies it got them adjacent -- and gives up loudly rather than
         * handing out a stack that silently ends after 4 KiB. */
        uint64_t base = pmm_alloc_page();
        if (!base)
            return -1;
        for (int p = 1; p < STACK_PAGES; p++) {
            uint64_t next = pmm_alloc_page();
            if (next != base + (uint64_t)p * PAGE_SIZE) {
                kprintf("sched: could not get %d contiguous stack pages for '%s'\n",
                        STACK_PAGES, name);
                return -1;
            }
        }

        threads[i].stack_phys = base;
        threads[i].name = name;
        threads[i].slices = 0;
        threads[i].done = false;

        /* The stack is reached through the direct map, which is mapped
         * PXN|UXN -- a stack that is executable is a stack you can be made to
         * return into. */
        uint64_t top = P2V(base) + (uint64_t)STACK_PAGES * PAGE_SIZE;
        threads[i].entry = entry;
        kernel_ctx_prepare(&threads[i].ctx, bringup_entry, top);

        threads[i].used = true;
        kprintf("sched: thread %d '%s' stack %p..%p\n", i, name,
                (void *)(uintptr_t)P2V(base), (void *)(uintptr_t)top);
        return i;
    }
    return -1;
}

void bringup_sched_tick(void) {
    if (!running || current < 0)
        return;

    /* Plain round robin: next runnable slot after this one. No priorities, no
     * fairness accounting -- process.c has all of that and this is not it. */
    int next = current;
    for (int n = 1; n <= MAX_THREADS; n++) {
        int cand = (current + n) % MAX_THREADS;
        if (threads[cand].used && !threads[cand].done) {
            next = cand;
            break;
        }
    }

    if (next == current)
        return;                 /* nothing else runnable: stay put */

    int prev = current;
    current = next;
    threads[next].slices++;

    /* Called from inside the timer IRQ handler, and it does not return here
     * until something switches back. The GIC has ALREADY been told the
     * interrupt is finished (see gic_dispatch) precisely so this is allowed. */
    /* No FP areas: every thread here is a KERNEL thread, and the kernel is
     * built -mgeneral-regs-only, so there is no floating-point state to lose.
     * A6's user threads pass real pointers. */
    kernel_ctx_switch(&threads[prev].ctx, &threads[next].ctx, 0, 0);
}

void kthread_exited(void) {
    if (current >= 0)
        threads[current].done = true;

    kprintf("sched: thread %d '%s' returned; parking\n",
            current, threads[current].name);

    /* Interrupts stay on so the next tick can schedule someone else. Marked
     * done above, so the round robin will not pick this thread again. */
    for (;;)
        __asm__ volatile("wfi");
}

void bringup_sched_stop(void) {
    running = false;
}

uint64_t bringup_thread_slices(int id) {
    return (id >= 0 && id < MAX_THREADS) ? threads[id].slices : 0;
}

int bringup_thread_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used)
            n++;
    return n;
}

void bringup_sched_dump(void) {
    kprintf("sched: %d threads\n", bringup_thread_count());
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used)
            kprintf("sched:   %d %-10s %d slices%s\n", i, threads[i].name,
                    (int)threads[i].slices, threads[i].done ? " (done)" : "");
}

uint64_t kernel_ctx_pc(const struct kcontext *ctx) { return ctx->pc; }
uint64_t kernel_ctx_fp(const struct kcontext *ctx) { return ctx->fp; }
