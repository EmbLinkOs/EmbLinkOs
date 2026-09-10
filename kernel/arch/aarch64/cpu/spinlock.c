#include "include/spinlock.h"

/* The aarch64 half of kernel/include/spinlock.h. The x86 implementation is
 * kernel/arch/x86_64/cpu/spinlock.c; the two exist to be read side by side,
 * because the SHAPE is identical and only the three machine-specific steps
 * differ:
 *
 *            x86_64                      aarch64
 *   save     pushfq / pop                mrs  x, daif
 *   mask     cli                         msr  daifset, #2
 *   restore  sti if IF was set           msr  daifclr, #2 if I was clear
 *   backoff  pause                       wfe  (with sev on unlock)
 *
 * The atomic itself is __atomic_exchange_n on both -- gcc emits LDAXR/STXR
 * here and LOCK XCHG there, and neither needs hand-written assembly.
 *
 * saved_flags holds PSTATE.DAIF rather than RFLAGS. Both are "the interrupt
 * state this lock has to put back", which is why the struct is shared. */

/* PSTATE.DAIF as read by `mrs`: D=9, A=8, I=7, F=6. Set means MASKED. */
#define DAIF_I (1UL << 7)

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->saved_flags = 0;
}

void spin_lock(spinlock_t *lock) {
    uint64_t flags;

    /* Read the interrupt state BEFORE masking, or we would restore "masked"
     * on unlock and never take another interrupt. */
    __asm__ volatile("mrs %0, daif" : "=r"(flags) :: "memory");
    __asm__ volatile("msr daifset, #2" ::: "memory");   /* mask IRQ only */

    uint64_t spun = 0;
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE) != 0) {
        /* WFE parks the core until an event arrives, which spin_unlock sends
         * with SEV. This is the ARM equivalent of x86's `pause` but stronger:
         * `pause` is a hint, WFE actually stops fetching. The race everyone
         * worries about -- SEV arriving between the failed exchange and the
         * WFE -- is handled by the architecture: SEV sets a per-core event
         * register, and WFE returns immediately when it is already set. */
        __asm__ volatile("wfe" ::: "memory");
        spun++;
    }

    /* Safe to write only now: until the exchange succeeded, another core could
     * have been the owner and this field is the owner's. */
    lock->saved_flags = flags;

    /* Accounting, written only by the holder -- see spinlock.h. A WFE spin
     * counts iterations rather than cycles, which on this architecture means
     * "how many times we were woken and still lost"; it is a contention
     * signal, not a duration. */
    lock->acquires++;
    if (spun) { lock->contended++; lock->spins += spun; }
}

void spin_unlock(spinlock_t *lock) {
    uint64_t flags = lock->saved_flags;

    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);

    /* Wake anyone in WFE above. Harmless if nobody is waiting. */
    __asm__ volatile("sev" ::: "memory");

    /* Restore, do not blindly enable: this lock may be held inside another. */
    if (!(flags & DAIF_I))
        __asm__ volatile("msr daifclr, #2" ::: "memory");
}
