#include "power/policy.h"
#include "acpi/power.h"
#include "acpi/display.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/io.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "drivers/timer/timer.h"
#include "process/ksync.h"
#include "block/block.h"
#include "fs/embkfs/embkfs.h"
#include <uacpi/uacpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>

static spinlock_t cache_lock=SPINLOCK_INIT;
static struct embk_power_info cache;
static uacpi_handle refresh_item;
static unsigned refresh_pending,started,ticks,power_action;
static int last_error,power_worker_ready;
static struct wait_queue power_wait;
struct reading {struct embk_power_info p;uint64_t remaining,full;unsigned known,unknown;};
static int integers(uacpi_namespace_node *n,const char *method,uint64_t *v,unsigned count) {
    uacpi_object *o=NULL;
    if(uacpi_eval_simple_package(n,method,&o)!=UACPI_STATUS_OK) return 0;
    uacpi_object_array a;
    int ok=uacpi_object_get_package(o,&a)==UACPI_STATUS_OK && a.count>=count;
    for(unsigned i=0;i<count && ok;i++) ok=uacpi_object_get_integer(a.objects[i],&v[i])==UACPI_STATUS_OK;
    uacpi_object_unref(o);return ok;
}
static uacpi_iteration_decision battery(void *ctx,uacpi_namespace_node *n,uacpi_u32 depth) {
    (void)depth;struct reading *r=ctx;uint64_t sta=0x1f,bst[4],bif[4];
    uacpi_status s=uacpi_eval_simple_integer(n,"_STA",&sta);
    if((s!=UACPI_STATUS_OK && s!=UACPI_STATUS_NOT_FOUND) || !(sta&0x10)) return UACPI_ITERATION_DECISION_CONTINUE;
    r->p.batteries++;r->p.flags|=EMBK_POWER_BATTERY;
    if(!integers(n,"_BST",bst,4)) {r->unknown++;return UACPI_ITERATION_DECISION_CONTINUE;}
    if(bst[0]<=7 && (bst[0]&1)) r->p.flags|=EMBK_POWER_DISCHARGING;
    if(bst[0]<=7 && (bst[0]&2)) r->p.flags|=EMBK_POWER_CHARGING;
    if(bst[0]<=7 && (bst[0]&4)) r->p.flags|=EMBK_POWER_CRITICAL;
    unsigned bix=integers(n,"_BIX",bif,4);
    if(!bix && !integers(n,"_BIF",bif,3)) {r->unknown++;return UACPI_ITERATION_DECISION_CONTINUE;}
    uint64_t unit=bif[bix?1:0],full=bif[bix?3:2];
    if(unit>1 || !full || full>=UINT32_MAX || bst[2]>=UINT32_MAX) {
        r->unknown++;return UACPI_ITERATION_DECISION_CONTINUE;
    }
    if(!r->known) {
        r->p.unit=unit;
        r->p.voltage=bst[3]<UINT32_MAX?bst[3]:EMBK_POWER_UNKNOWN;
        r->p.rate=bst[1]<UINT32_MAX?bst[1]:EMBK_POWER_UNKNOWN;
    } else {
        if(r->p.unit!=unit) r->unknown++;
        r->p.voltage=r->p.rate=EMBK_POWER_UNKNOWN;
    }
    r->known++;r->remaining+=bst[2];r->full+=full;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision adapter(void *ctx,uacpi_namespace_node *n,uacpi_u32 depth) {
    (void)depth;struct reading *r=ctx;uint64_t online;
    if(uacpi_eval_simple_integer(n,"_PSR",&online)==UACPI_STATUS_OK && online<=1) {
        r->p.flags|=EMBK_POWER_AC_KNOWN;
        if(online) r->p.flags|=EMBK_POWER_AC_ONLINE;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision lid(void *ctx,uacpi_namespace_node *n,uacpi_u32 depth) {
    (void)depth;struct reading *r=ctx;uint64_t open;
    if(uacpi_eval_simple_integer(n,"_LID",&open)==UACPI_STATUS_OK && open<=1) {
        r->p.flags|=EMBK_POWER_LID_KNOWN;
        if(!open) r->p.flags|=EMBK_POWER_LID_CLOSED;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static void refresh(uacpi_handle ctx) {
    (void)ctx;
    struct reading r={0};r.p.version=EMBK_POWER_VERSION;
    r.p.percent=r.p.remaining=r.p.full=r.p.rate=r.p.voltage=r.p.unit=EMBK_POWER_UNKNOWN;
    uacpi_find_devices("PNP0C0A",battery,&r);
    uacpi_find_devices("ACPI0003",adapter,&r);
    uacpi_find_devices("PNP0C0D",lid,&r);
    acpi_display_refresh();
    if(r.known && !r.unknown && r.full) {
        uint64_t percent=r.remaining*100/r.full;
        r.p.percent=percent>100?100:percent;
        if(r.remaining<UINT32_MAX && r.full<UINT32_MAX) {r.p.remaining=r.remaining;r.p.full=r.full;}
    } else r.p.unit=r.p.rate=r.p.voltage=EMBK_POWER_UNKNOWN;
    r.p.sampled_ms=timer_uptime_ms();
    spin_lock(&cache_lock);cache=r.p;spin_unlock(&cache_lock);
    __atomic_store_n(&refresh_pending,0,__ATOMIC_RELEASE);
    power_policy_poll();
}
int acpi_power_snapshot(struct embk_power_info *out) {
    if(!acpi_runtime_ready()) return -EMBK_ENODEV;
    spin_lock(&cache_lock);*out=cache;spin_unlock(&cache_lock);
    out->last_error=__atomic_load_n(&last_error,__ATOMIC_ACQUIRE);
    if(__atomic_load_n(&power_action,__ATOMIC_ACQUIRE)) out->flags|=EMBK_POWER_BUSY;
    return 0;
}
void acpi_power_tick(void) {
    if(!__atomic_load_n(&started,__ATOMIC_ACQUIRE) || ++ticks<200) return;
    ticks=0;unsigned expected=0;
    if(__atomic_compare_exchange_n(&refresh_pending,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION,refresh_item,refresh,NULL);
}
static int transition(unsigned action) {
    /* Firmware methods execute with IRQs enabled. The final register write
     * happens after root transactions finish and all new disk writes stop. */
    uacpi_status s=UACPI_STATUS_OK;
    embkfs_lock();embk_block_freeze_writes();
    int rc=embk_block_sync_dirty();
    /* A disk failure must not run _PTS and partly power down firmware devices. */
    if(!rc && action==EMBK_POWER_OFF) {
        s=uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
        if(s!=UACPI_STATUS_OK) rc=-EMBK_ENOTSUP;
    }
    if(!rc) {
        kprintf("power: storage synchronized; %s\n",action==EMBK_POWER_OFF?"powering off":"rebooting");
        uacpi_interrupt_state flags=uacpi_kernel_disable_interrupts();
        if(action==EMBK_POWER_OFF) s=uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
        else {
            s=uacpi_reboot();
            /* Legacy reset fallback if FADT ResetReg is absent/ineffective. */
            uint64_t start=time_get_us();
            while((inb(0x64)&2) && time_get_us()-start<100000) __asm__ volatile("pause");
            if(!(inb(0x64)&2)) outb(0x64,0xfe);
        }
        uacpi_kernel_restore_interrupts(flags);
        rc=-EMBK_EIO; /* A successful transition cannot return here. */
    }
    embk_block_thaw_writes();embkfs_unlock();
    return rc;
}
static void power_worker(void) {
    sched_lock();current_thread->pinned_cpu=0;sched_unlock();schedule();
    for(;;) {
        sched_lock();
        unsigned action=__atomic_load_n(&power_action,__ATOMIC_ACQUIRE);
        if(!action) {sched_block_current_locked(&power_wait);continue;}
        sched_unlock();
        int rc=transition(action);
        __atomic_store_n(&last_error,rc,__ATOMIC_RELEASE);
        __atomic_store_n(&power_action,0,__ATOMIC_RELEASE);
        kprintf("power: transition failed (%d); disk writes restored\n",rc);
    }
}
int acpi_power_request(unsigned action) {
    if(action!=EMBK_POWER_OFF && action!=EMBK_POWER_REBOOT) return -EMBK_EINVAL;
    if(!power_worker_ready) return -EMBK_ENODEV;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&power_action,&expected,action,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return -EMBK_EBUSY;
    __atomic_store_n(&last_error,0,__ATOMIC_RELEASE);
    sched_lock();wait_queue_wake_one(&power_wait);sched_unlock();return 0;
}
void acpi_power_init(void) {
    refresh(NULL);
    refresh_item=uacpi_kernel_create_work_item();
    power_worker_ready=process_create_kthread(power_worker,NULL)!=NULL;
    if(refresh_item) __atomic_store_n(&started,1,__ATOMIC_RELEASE);
}
