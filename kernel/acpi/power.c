/* Laptop telemetry uses the upstream AML namespace. Cached snapshots never
 * run firmware in a UI syscall; orderly transitions stay in power/power.c. */
#include "acpi/power.h"
#include "acpi/display.h"
#include "acpi/aml.h"
#include "power/power.h"
#include "power/policy.h"
#include "process/process.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "include/errno.h"
static spinlock_t cache_lock=SPINLOCK_INIT;
static struct embk_power_info cache={.version=EMBK_POWER_VERSION,.percent=EMBK_POWER_UNKNOWN,
    .remaining=EMBK_POWER_UNKNOWN,.full=EMBK_POWER_UNKNOWN,.rate=EMBK_POWER_UNKNOWN,
    .voltage=EMBK_POWER_UNKNOWN,.unit=EMBK_POWER_UNKNOWN};
static int last_error;
struct reading {struct embk_power_info p;uint64_t remaining,full;unsigned known,unknown;};
static int integers(struct aml_node *n,const char *method,uint64_t *v,unsigned count) {
    struct aml_node *m=aml_lookup(n,method);
    struct aml_object *o=m && m->parent==n?aml_evaluate(m,NULL,0):NULL;
    int ok=o && o->kind==AML_PACKAGE && o->u.pkg.count>=count;
    for(unsigned i=0;i<count && ok;i++) {
        struct aml_object *e=o->u.pkg.elem[i];
        ok=e && e->kind==AML_INTEGER;
        if(ok)v[i]=e->u.integer;
    }
    aml_put(o);return ok;
}
static bool battery(struct aml_node *n,void *ctx) {
    struct reading *r=ctx;uint64_t sta=0x1f,bst[4],bif[4];
    struct aml_node *status=aml_lookup(n,"_STA");
    if((status && status->parent==n && !aml_eval_integer(n,"_STA",&sta)) || !(sta&0x10)) return true;
    r->p.batteries++;r->p.flags|=EMBK_POWER_BATTERY;
    if(!integers(n,"_BST",bst,4)) {r->unknown++;return true;}
    if(bst[0]<=7 && (bst[0]&1)) r->p.flags|=EMBK_POWER_DISCHARGING;
    if(bst[0]<=7 && (bst[0]&2)) r->p.flags|=EMBK_POWER_CHARGING;
    if(bst[0]<=7 && (bst[0]&4)) r->p.flags|=EMBK_POWER_CRITICAL;
    unsigned bix=integers(n,"_BIX",bif,4);
    if(!bix && !integers(n,"_BIF",bif,3)) {r->unknown++;return true;}
    uint64_t unit=bif[bix?1:0],full=bif[bix?3:2];
    if(unit>1 || !full || full>=UINT32_MAX || bst[2]>=UINT32_MAX) {
        r->unknown++;return true;
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
    return true;
}
static bool adapter(struct aml_node *n,void *ctx) {
    struct reading *r=ctx;uint64_t online;
    if(aml_eval_integer(n,"_PSR",&online) && online<=1) {
        r->p.flags|=EMBK_POWER_AC_KNOWN;
        if(online) r->p.flags|=EMBK_POWER_AC_ONLINE;
    }
    return true;
}
static bool lid(struct aml_node *n,void *ctx) {
    struct reading *r=ctx;uint64_t open;
    if(aml_eval_integer(n,"_LID",&open) && open<=1) {
        r->p.flags|=EMBK_POWER_LID_KNOWN;
        if(!open) r->p.flags|=EMBK_POWER_LID_CLOSED;
    }
    return true;
}

static void refresh(void) {
    struct reading r={0};r.p.version=EMBK_POWER_VERSION;
    r.p.percent=r.p.remaining=r.p.full=r.p.rate=r.p.voltage=r.p.unit=EMBK_POWER_UNKNOWN;
    aml_for_each_hid("PNP0C0A",battery,&r);
    aml_for_each_hid("ACPI0003",adapter,&r);
    aml_for_each_hid("PNP0C0D",lid,&r);
    if(r.known && !r.unknown && r.full) {
        uint64_t percent=r.remaining*100/r.full;
        r.p.percent=percent>100?100:percent;
        if(r.remaining<UINT32_MAX && r.full<UINT32_MAX) {r.p.remaining=r.remaining;r.p.full=r.full;}
    } else r.p.unit=r.p.rate=r.p.voltage=EMBK_POWER_UNKNOWN;
    r.p.sampled_ms=timer_uptime_ms();
    spin_lock(&cache_lock);cache=r.p;spin_unlock(&cache_lock);
}
static void worker(void) {
    uint64_t next=0;
    for(;;) {
        uint64_t now=timer_uptime_ms();
        if(now>=next){refresh();next=now+1000;}
        acpi_display_refresh();power_policy_poll();sched_sleep_ms(250);
    }
}
int acpi_runtime_ready(void) {return aml_available();}
void acpi_power_init(void) {acpi_display_init();process_create_kthread(worker,0);}
void acpi_power_tick(void) {}
int acpi_power_snapshot(struct embk_power_info *out) {
    if(!aml_available())return -EMBK_ENODEV;
    spin_lock(&cache_lock);*out=cache;spin_unlock(&cache_lock);
    out->last_error=__atomic_load_n(&last_error,__ATOMIC_ACQUIRE);return 0;
}
int acpi_power_request(unsigned action) {
    if(action!=EMBK_POWER_OFF && action!=EMBK_POWER_REBOOT)return -EMBK_EINVAL;
    int rc=power_transition(action==EMBK_POWER_OFF?POWER_OFF:POWER_REBOOT);
    __atomic_store_n(&last_error,rc,__ATOMIC_RELEASE);return rc;
}
