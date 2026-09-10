#include "kworker.h"
#include "process/process.h"
#include "include/kprintf.h"
#include "mm/vm_object.h"   /* the page object rides along with the vnode */
#include <stdint.h>

#define DEFERRED_VN_MAX 64   /* max number of vnode obj_puts we can queue at once */

/* One deferred teardown: release the file's page object, THEN the vnode.
 * That order is the contract -- flushing dirty pages needs the object still to
 * exist on the filesystem, and obj_put is what may destroy it. */
struct deferred_close {
    struct vnode vn;
    struct vm_object *obj;       /* may be NULL: not every fd is cached */
};

static struct deferred_close g_ring[DEFERRED_VN_MAX];
static uint32_t     g_head, g_tail;              /* guarded by g_sched_lock */
static struct wait_queue    g_kworker_wq;        /* gzero_init: NULL head is empty */


void kworker_defer_vmo_put_locked(struct vnode vn, struct vm_object *obj) {
    uint32_t next = (g_head + 1) % DEFERRED_VN_MAX;
    if (next == g_tail) {
        /* Ring full: log + leak this one. See header contract */
        kprintf("kworker: deferred ring full, leaking one obj_put (ino=%lu)\n", vn.ino);
        return;
    }
    g_ring[g_head].vn  = vn;       /* struct copy -- vnode is small + copyable */
    g_ring[g_head].obj = obj;
    g_head = next;
    wait_queue_wake_one(&g_kworker_wq);   /* safe: caller holds g_sched_lock,
                                           * same shape as keyboard_deliver */
}

void kworker_defer_obj_put_locked(struct vnode vn) {
    kworker_defer_vmo_put_locked(vn, NULL);
}

uint32_t kworker_pending(void) {
    return (g_head - g_tail) % DEFERRED_VN_MAX;   /* modular ring occupancy */
}


static void kworker_main(void) {
    while (1) {
        struct deferred_close job;

        sched_lock();
        while (g_head == g_tail) {
            /* sched_block_current_locked RETURNS UNLOCKED (it released the
             * lock to switch away) -- re-LOCK before re-checking emptiness,
             * exactly keyboard_getchar_blocking's discipline. Unlocking here
             * instead would double-release the spinlock AND read the ring
             * without the lock. */
            sched_block_current_locked(&g_kworker_wq);
            sched_lock();
        }
        job = g_ring[g_tail];
        g_tail = (g_tail + 1) % DEFERRED_VN_MAX;
        sched_unlock();

        /* No locks held. This MAY BLOCK ON DISK (last-close reads the on-disk
         * link count; unlinked-while-open destroys blocks + writes metadata)
         * -- and that's the entire reason this thread exists: we're a normal
         * schedulable thread holding nothing, so blocking is fine.
         *
         * Concurrency note, scoped precisely: this obj_put races syscall-path
         * obj_puts from other cores on embkfs's bare-static g_open_refs. That
         * race EXISTS TODAY (two processes closing files on two cores); the
         * kworker adds one more racer to an already-ledgered SMP hazard, it
         * does not create it. Tracked with the other embkfs statics. */       

        /* The page object first: its flush needs the file to still exist, and
         * the obj_put below is what may destroy it. Both may block on disk,
         * which is the entire reason this thread exists. */
        vmo_put(job.obj);

        if (job.vn.mnt && job.vn.mnt->ops && job.vn.mnt->ops->obj_put) {
            (void)job.vn.mnt->ops->obj_put(job.vn.mnt, job.vn.ino);
        }
    }
}


void kworker_init(void) {
    g_head = g_tail = 0;
    
    process_create_kthread(kworker_main, NULL);    /* fire-and-forget, loops
                                                    * forever -- same shape
                                                    * as the per-core idle
                                                    * kthread */
    kprintf("kworker: deferred-teardown thread started\n");
}