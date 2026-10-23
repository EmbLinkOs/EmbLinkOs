#include "kworker.h"
#include "process/process.h"
#include "include/kprintf.h"
#include "mm/vm_object.h"   /* the page object rides along with the vnode */
#include "mm/vma.h"         /* vma_destroy_list: a dead process's mappings */
#include "mm/vmm.h"         /* vmm_destroy_address_space */
#include <stdint.h>

/* The ring. 256 rather than the 64 it started at: every process exit is now
 * a job (its address space), and a burst of exits -- a test spawning and
 * killing a dozen children, a shell pipeline -- must not have to leak one.
 * Overflow still degrades to a logged leak rather than blocking a reap path
 * that holds the scheduler lock. */
#define DEFERRED_MAX 256

enum job_kind { JOB_VMO_PUT, JOB_ADDRESS_SPACE, JOB_SESSION_END };

/* One deferred teardown. JOB_VMO_PUT: release the file's page object, THEN
 * the vnode -- that order is the contract, because flushing dirty pages needs
 * the object still to exist on the filesystem, and obj_put is what may destroy
 * it. JOB_ADDRESS_SPACE: the mappings first (their objects let go of the
 * frames they still hold), then the page tables. */
struct deferred_job {
    enum job_kind kind;
    struct vnode vn;
    struct vm_object *obj;       /* may be NULL: not every fd is cached */
    uint64_t pml4;
    struct vm_area *vmas;
    uint32_t session;
};

static struct deferred_job g_ring[DEFERRED_MAX];
static uint32_t     g_head, g_tail;              /* guarded by g_sched_lock */
static bool         g_busy;                      /* a job dequeued and not yet finished */
static struct wait_queue    g_kworker_wq;        /* gzero_init: NULL head is empty */

/* Caller holds g_sched_lock. Returns the slot to fill, or NULL when full. */
static struct deferred_job *ring_push_locked(void) {
    uint32_t next = (g_head + 1) % DEFERRED_MAX;
    if (next == g_tail)
        return NULL;
    struct deferred_job *j = &g_ring[g_head];
    g_head = next;
    return j;
}

void kworker_defer_vmo_put_locked(struct vnode vn, struct vm_object *obj) {
    struct deferred_job *j = ring_push_locked();
    if (!j) {
        /* Ring full: log + leak this one. See header contract */
        kprintf("kworker: deferred ring full, leaking one obj_put (ino=%lu)\n", vn.ino);
        return;
    }
    j->kind = JOB_VMO_PUT;
    j->vn   = vn;       /* struct copy -- vnode is small + copyable */
    j->obj  = obj;
    wait_queue_wake_one(&g_kworker_wq);   /* safe: caller holds g_sched_lock,
                                           * same shape as keyboard_deliver */
}

void kworker_defer_obj_put_locked(struct vnode vn) {
    kworker_defer_vmo_put_locked(vn, NULL);
}

void kworker_defer_address_space_locked(uint64_t pml4_phys, struct vm_area *vmas) {
    struct deferred_job *j = ring_push_locked();
    if (!j) {
        kprintf("kworker: deferred ring full, LEAKING an address space (pml4 %llx)\n",
                (unsigned long long)pml4_phys);
        return;
    }
    j->kind = JOB_ADDRESS_SPACE;
    j->pml4 = pml4_phys;
    j->vmas = vmas;
    wait_queue_wake_one(&g_kworker_wq);
}

void kworker_defer_session_end_locked(uint32_t sid) {
    struct deferred_job *j = ring_push_locked();
    if (!j) {
        kprintf("kworker: deferred ring full -- session %u's stragglers are left running\n",
                (unsigned)sid);
        return;
    }
    j->kind = JOB_SESSION_END;
    j->session = sid;
    wait_queue_wake_one(&g_kworker_wq);
}

uint32_t kworker_pending(void) {
    /* Ring occupancy PLUS the job in flight: a caller waiting for "nothing
     * left to do" (test swap, before it counts what came back) must not be
     * told zero while an address space is still being torn down. */
    return (g_head - g_tail) % DEFERRED_MAX + (g_busy ? 1u : 0u);
}


static void kworker_main(void) {
    while (1) {
        struct deferred_job job;

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
        g_tail = (g_tail + 1) % DEFERRED_MAX;
        g_busy = true;
        sched_unlock();

        /* No locks held. This MAY BLOCK ON DISK -- and that's the entire
         * reason this thread exists: we're a normal schedulable thread holding
         * nothing, so blocking is fine. */
        switch (job.kind) {
        case JOB_VMO_PUT:
            /* The page object first: its flush needs the file to still exist,
             * and the obj_put below is what may destroy it. Both may block on
             * disk. (last-close reads the on-disk link count; unlinked-while-
             * open destroys blocks + writes metadata.)
             *
             * Concurrency note, scoped precisely: this obj_put races
             * syscall-path obj_puts from other cores on embkfs's bare-static
             * g_open_refs. That race EXISTS TODAY (two processes closing files
             * on two cores); the kworker adds one more racer to an already-
             * ledgered SMP hazard, it does not create it. */
            vmo_put(job.obj);
            if (job.vn.mnt && job.vn.mnt->ops && job.vn.mnt->ops->obj_put)
                (void)job.vn.mnt->ops->obj_put(job.vn.mnt, job.vn.ino);
            break;

        case JOB_ADDRESS_SPACE:
            /* Mappings first: each object takes its pages out of these tables
             * and drops the mapping's reference (a file object's last one
             * flushes -- disk). THEN the tables, which free every frame still
             * under them: the ELF segments, and nothing that belongs to an
             * object. */
            vma_destroy_list(job.vmas, job.pml4);
            vmm_destroy_address_space(job.pml4);
            break;

        case JOB_SESSION_END: {
            /* The leader died on its own (a crash, or exit without logout):
             * the session is over, and anything it left running goes with
             * it. The kworker is in the system session, so it is never one of
             * the victims and session_end returns. */
            int n = session_end(job.session, false);
            if (n > 0)
                kprintf("session %u ended with its leader: %d process(es) it left running were stopped\n",
                        (unsigned)job.session, n);
            break;
        }
        }
        sched_lock();
        g_busy = false;
        sched_unlock();
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
