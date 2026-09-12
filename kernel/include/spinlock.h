#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include "include/types.h"
#include <stdint.h>



// A spinlock is just a flag plus saved interrupt state.

typedef struct spinlock {
    volatile uint32_t locked; // 0 = unlocked, 1 = locked
    uint64_t saved_flags;   // saved RFLAGS for restoring interrupt state on unlock

    /* --- contention accounting -------------------------------------------
     *
     * "This lock is a bottleneck" is a claim, and claims here get measured.
     * Per-core run queues are the standard answer to a hot scheduler lock,
     * and they are a large change to the most dangerous code in the kernel --
     * worth making only against a number, not a hunch.
     *
     * NO ATOMICS, and that is what makes this cheap enough to leave on. Every
     * one of these is written ONLY BY THE THREAD THAT HOLDS THE LOCK, after it
     * has been acquired: the holder is unique by definition, so a plain
     * increment on a cacheline it already owns exclusively cannot race. The
     * spin counts are accumulated in a local while waiting and folded in
     * afterwards, so the waiting path adds nothing either.
     *
     * The uncontended cost is therefore one increment of a line that is
     * already hot and already dirty -- which is as close to free as an
     * always-on counter gets. */
    uint64_t acquires;      /* every successful acquisition                 */
    uint64_t contended;     /* ...that had to spin at least once            */
    uint64_t spins;         /* total spin iterations across all waiters     */

    /* WHO HOLDS IT, and from where. Written by the holder right after it wins
     * the lock and cleared just before it lets go, so the pair is meaningful
     * exactly while `locked` is 1 -- and meaningless, deliberately, when it is
     * 0 (holder_cpu = -1).
     *
     * This exists because of a deadlock that could not be diagnosed without
     * it: a boot stops with ALL FOUR cores spinning in spin_lock, which says
     * only that somebody never unlocked. Nothing in the machine's state says
     * WHO -- the holder is not running, so no core's PC points at it. Two
     * One word written by a core that already owns the cacheline turns
     * "somebody" into a named call site. Same cost argument as the counters
     * above: no atomics, holder-only writes. The CPU number is deliberately
     * NOT recorded -- reading it means reaching into per-CPU data, which is
     * built on top of locks, and the call site is the half that identifies the
     * bug anyway. */
    uint64_t holder_lr;     /* return address of whoever holds it; 0 = free */
} spinlock_t;

// static initializer for spinlocks
#define SPINLOCK_INIT { 0, 0, 0, 0, 0, 0 }

/* A snapshot. Racy by construction (the numbers move while being read) and
 * that is fine: they are ratios, not ledgers. */
static inline void spin_stats(const spinlock_t *l, uint64_t *acq,
                              uint64_t *cont, uint64_t *spins) {
    if (acq)   *acq   = l->acquires;
    if (cont)  *cont  = l->contended;
    if (spins) *spins = l->spins;
}


// Initialize a spinlock (set locked to 0)
void spinlock_init(spinlock_t *lock);

// Acquire the spinlock. Disables interrupts and saves previous state in lock->saved_rflags.
// If the lock is already held, it will spin until it becomes available.
void spin_lock(spinlock_t *lock);

// Release the spinlock. Restores previous interrupt state from lock->saved_rflags.
void spin_unlock(spinlock_t *lock);



#endif /* __SPINLOCK_H__ */