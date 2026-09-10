#ifndef _FUTEX_H_
#define _FUTEX_H_

#include <stdint.h>

/* See futex.c. The fast path of a userland lock never reaches here; these two
 * calls are only what a thread does when it must actually wait, and what the
 * releaser does when somebody might be waiting. */

/* Atomically: if the 32-bit word at `uaddr` still equals `val`, sleep until
 * woken. Returns 0 when woken (possibly spuriously -- the caller re-checks its
 * own condition), -EMBK_EAGAIN if the word had already changed, -EMBK_EFAULT
 * if the address is not the caller's, or -EMBK_ECANCELED if the process was
 * cancelled while waiting. */
int64_t futex_wait(const void *uaddr, uint32_t val);

/* Wake up to `n` threads waiting on `uaddr`. Returns how many were woken --
 * which is information the caller can use, and which a lock's slow path uses
 * to decide whether it needs to wake anyone else. */
int64_t futex_wake(const void *uaddr, uint32_t n);

/* Counters, for `test futex`. `waits` that never became `wakes` is a lost
 * wakeup; `eagain` is the race the compare-inside-the-kernel catches, and a
 * zero there under contention means the test never actually raced. */
void futex_stats(uint64_t *waits, uint64_t *wakes, uint64_t *eagain);

#endif /* _FUTEX_H_ */
