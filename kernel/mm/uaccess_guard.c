#include "include/uaccess_guard.h"
#include "include/arch_thread.h"     /* struct kcontext + kernel_ctx_save/restore */
#include "process/process.h"

/* See include/uaccess_guard.h. The whole implementation is "where does the
 * saved context live", and the answer is: in the thread, because the copy it
 * protects is preemptible. */

static uint64_t g_recoveries;

void uaccess_disarm(void) {
    struct thread *t = current_thread;
    if (t)
        t->uaccess_armed = false;
}

bool uaccess_fault_recover(void) {
    struct thread *t = current_thread;
    if (!t || !t->uaccess_armed)
        return false;

    /* Disarm BEFORE jumping. The resumed code disarms too, but if the jump
     * itself faulted we would otherwise loop forever between the fault handler
     * and a guard that still looks armed. */
    t->uaccess_armed = false;
    g_recoveries++;

    kernel_ctx_restore(&t->uaccess_ctx, 1);
    return true;                    /* not reached: restore does not return */
}

uint64_t uaccess_recoveries(void) { return g_recoveries; }
