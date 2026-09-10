#ifndef __KWORKER_H__
#define __KWORKER_H__

#include "fs/vfs.h"

/* Start the deferred work kernel thread. Call once at boot, after the
 * scheduler + timer are initialized (main.c, after the idle-kthread loop,
 * before the home spawn -- the worker must exist before that first process
 * that could ever exist). */
void kworker_init(void);

/* Defer a vnode obj_put to kworker. Must be called with g_sched_lock held.
 * (it touches the ring and the wait queue, both guarded by it). Returns
 * void BY CONTRACT: the caller (the reap loop) must never fail or branch
 * on ring state. If the ring is full, we log and leak THAT ONE entry --
 * which is exactly the pre-existing behavior for EVERY entry, so overflow
 * degrades to the status quo, bounded and honest. */

void kworker_defer_obj_put_locked(struct vnode vn);

/* The same, plus the file's page object (mm/vm_object.h). The fd close path
 * that runs under g_sched_lock cannot call vmo_put itself: it takes a sleeping
 * lock and may write dirty pages to the disk. The worker releases the page
 * object FIRST and the vnode second -- flushing needs the file to still exist,
 * and obj_put is what may destroy it. `obj` may be NULL. */
struct vm_object;
void kworker_defer_vmo_put_locked(struct vnode vn, struct vm_object *obj);

/* EmbDBG v2 Kernel Object Explorer: how many deferred-teardown items are queued
 * right now (g_head - g_tail). A racy debug sample; no lock. */
uint32_t kworker_pending(void);

#endif   /* __KWORKER_H__ */