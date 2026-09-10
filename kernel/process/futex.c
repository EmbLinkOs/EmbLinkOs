#include "process/futex.h"
#include "process/process.h"
#include "include/errno.h"
#include "include/usercopy.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "include/spinlock.h"

/* ==========================================================================
 * FUTEX -- how a userland lock waits without spinning.
 *
 * THE HOLE THIS FILLS. Userland has had threads since sys_thread_create
 * existed, and no way whatsoever for two of them to agree about anything. A
 * program with a shared counter could spin on an atomic -- burning a whole
 * timeslice per contention, on a machine where the holder may be on another
 * core or may not be running at all -- or it could be wrong. There was no
 * third option.
 *
 * WHAT A FUTEX IS, and why it is shaped so oddly: the FAST path never enters
 * the kernel at all. An uncontended lock is one atomic compare-and-swap in
 * ring 3, and the kernel hears nothing. Only when a thread must actually WAIT
 * does it call in -- which is why the operation is not "lock this" but the
 * strange-looking "sleep if this word still says what I think it says".
 *
 *   WAIT(addr, val)  -- atomically: if *addr == val, sleep. Otherwise return
 *                       immediately with EAGAIN, because the world changed
 *                       between the caller's check and this call and sleeping
 *                       would be sleeping on stale information.
 *   WAKE(addr, n)    -- wake up to n sleepers on addr. Returns how many.
 *
 * That compare-inside-the-kernel is the entire trick. Without it there is a
 * window between "I saw the lock was held" and "I went to sleep" in which the
 * holder releases and wakes nobody, and the sleeper waits forever. The compare
 * and the enqueue happen under one lock here, and the waker takes the same
 * lock, so the window does not exist.
 *
 * THE KEY IS THE PHYSICAL ADDRESS, not the virtual one. Two processes sharing
 * a file mapping see the same word at different virtual addresses, and a futex
 * keyed on the virtual address would put them in different queues -- each
 * waiting for a wake the other believes it sent. Keying on the frame makes a
 * shared-memory lock work for free, and costs nothing for a private one, since
 * private pages have different frames anyway.
 * ========================================================================== */

#define FUTEX_BUCKETS 64

static struct wait_queue g_buckets[FUTEX_BUCKETS];

/* One lock for every bucket. Contention here is contention between *waiters*,
 * which by construction are already going to sleep -- the fast path never
 * reaches this file. A lock per bucket would be the shape to reach for if a
 * profile ever showed otherwise. */
static spinlock_t g_futex_lock = SPINLOCK_INIT;

static uint64_t g_waits, g_wakes, g_eagain;

static inline unsigned bucket_of(uint64_t key) {
    /* The low 12 bits are the offset within the page and the next bits are
     * the frame number; mixing both spreads locks that share a page. */
    return (unsigned)(((key >> 12) ^ (key >> 4) ^ key) % FUTEX_BUCKETS);
}

/* The frame-relative address of the user word, or 0 if it is not addressable.
 *
 * access_ok() is what makes this safe AND what faults the page in: with demand
 * paging a lock word in a freshly mmap'd region may have no page yet, and a
 * futex that refused it would make every new lock unusable until something
 * else happened to touch it. */
static uint64_t futex_key(const void *uaddr) {
    uint64_t va = (uint64_t)(uintptr_t)uaddr;
    if (va & 3)
        return 0;                    /* a 32-bit word must be 4-byte aligned */
    if (!access_ok(uaddr, 4))
        return 0;

    struct process *p = current_process_atomic();
    if (!p)
        return 0;
    uint64_t phys = vmm_get_phys_in(p->pml4_phys, va & ~(uint64_t)(PAGE_SIZE - 1));
    if (!phys)
        return 0;
    return phys | (va & (PAGE_SIZE - 1));
}

int64_t futex_wait(const void *uaddr, uint32_t val) {
    uint64_t key = futex_key(uaddr);
    if (!key)
        return -EMBK_EFAULT;

    /* Read the word THROUGH THE DIRECT MAP rather than copy_from_user. Not an
     * optimisation: the compare has to happen with the futex lock held, and
     * copy_from_user can take faults and other locks. The frame is already
     * resolved above, so this is the same bytes by a shorter road. */
    volatile const uint32_t *word = (volatile const uint32_t *)(uintptr_t)P2V(key);

    sched_lock();
    spin_lock(&g_futex_lock);

    if (*word != val) {
        /* The world moved between the caller's check and this call. Sleeping
         * now would be sleeping on information already known to be stale --
         * and this is the check that makes the whole protocol race-free. */
        g_eagain++;
        spin_unlock(&g_futex_lock);
        sched_unlock();
        return -EMBK_EAGAIN;
    }

    struct thread *t = current_thread;
    if (!t) {
        spin_unlock(&g_futex_lock);
        sched_unlock();
        return -EMBK_EPERM;
    }
    t->futex_key = key;
    g_waits++;

    spin_unlock(&g_futex_lock);
    /* Returns with g_sched_lock RELEASED, once something wakes us. */
    sched_block_current_locked(&g_buckets[bucket_of(key)]);

    t->futex_key = 0;

    /* A cancelled process's blocked threads are woken so they can notice. A
     * futex wait that came back for that reason must SAY so rather than
     * looking like a spurious wake the caller should retry -- retrying is
     * exactly what a cancelled thread must not do. */
    if (t->proc && t->proc->cancelled)
        return -EMBK_ECANCELED;

    /* Otherwise: woken, or woken spuriously. Both return 0, and the caller
     * re-checks its own condition -- which every correct futex user does
     * anyway, because a wake is a hint that the word MAY have changed, never a
     * promise that it did. */
    return 0;
}

int64_t futex_wake(const void *uaddr, uint32_t n) {
    uint64_t key = futex_key(uaddr);
    if (!key)
        return -EMBK_EFAULT;
    if (n == 0)
        return 0;

    sched_lock();
    spin_lock(&g_futex_lock);

    struct wait_queue *wq = &g_buckets[bucket_of(key)];
    uint32_t woken = 0;

    /* Walk the bucket and wake only the threads whose key MATCHES. A bucket
     * holds every futex that hashes to it, so waking the queue head blindly
     * would wake a thread waiting on a completely unrelated lock -- which is
     * not merely wasteful: that thread would re-check its own condition, find
     * it unchanged, and sleep again, while the thread that was actually
     * signalled sleeps through it. */
    struct thread *t = wq->head;
    while (t && woken < n) {
        struct thread *next = t->wait_next;
        if (t->futex_key == key) {
            wait_queue_remove(wq, t);
            t->state = PROCESS_READY;
            woken++;
        }
        t = next;
    }

    g_wakes += woken;
    spin_unlock(&g_futex_lock);
    sched_unlock();
    return (int64_t)woken;
}

void futex_stats(uint64_t *waits, uint64_t *wakes, uint64_t *eagain) {
    if (waits)  *waits  = g_waits;
    if (wakes)  *wakes  = g_wakes;
    if (eagain) *eagain = g_eagain;
}
