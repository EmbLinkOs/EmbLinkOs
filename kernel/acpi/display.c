/* Standard ACPI backlight methods, evaluated by the laptop telemetry worker.
 * Keep the upstream namespace as the sole owner of firmware execution. */
#include "acpi/display.h"
#include "acpi/aml.h"
#include "drivers/video/intel_display.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "include/errno.h"
static spinlock_t lock=SPINLOCK_INIT;
static struct embk_display_info cache={.version=EMBK_DISPLAY_VERSION};
static struct aml_node *panel;
static unsigned pending,target,native;

static struct aml_node *method(struct aml_node *node,const char *name) {
    struct aml_node *m=aml_lookup(node,name);
    return m && m->parent==node?m:0;
}
static bool discover(struct aml_node *node,void *ctx) {
    (void)ctx;
    if(panel || !node->value || node->value->kind!=AML_DEVICE ||
       !method(node,"_BCM") || !method(node,"_BQC"))return true;
    struct aml_object *o=aml_evaluate(method(node,"_BCL"),0,0);
    struct embk_display_info v={.version=EMBK_DISPLAY_VERSION};
    int valid=o && o->kind==AML_PACKAGE && o->u.pkg.count>=3 && o->u.pkg.count<=34;
    for(unsigned i=0;valid && i<o->u.pkg.count;i++) {
        struct aml_object *e=o->u.pkg.elem[i];
        if(!e || e->kind!=AML_INTEGER || e->u.integer>100){valid=0;break;}
        if(i<2)continue;
        unsigned j=0;while(j<v.level_count && v.levels[j]!=e->u.integer)j++;
        if(j==v.level_count)v.levels[v.level_count++]=e->u.integer;
    }
    aml_put(o);
    if(valid && v.level_count){panel=node;v.flags=EMBK_DISPLAY_BACKLIGHT;cache=v;}
    return true;
}
void acpi_display_init(void) {
    if(aml_available())aml_walk(aml_root(),discover,0);
    if(!panel) {
        unsigned value;
        if(!intel_backlight_get(&value)) {
            native=1;cache.flags=EMBK_DISPLAY_BACKLIGHT|EMBK_DISPLAY_NATIVE;
            for(unsigned i=0;i<=100;i+=5)cache.levels[cache.level_count++]=i;
        }
    }
    acpi_display_refresh();
}
void acpi_display_refresh(void) {
    unsigned apply=__atomic_load_n(&pending,__ATOMIC_ACQUIRE);
    unsigned value=0,wanted=__atomic_load_n(&target,__ATOMIC_ACQUIRE);
    int rc=0,known=0;
    if(native) {
        if(apply)rc=intel_backlight_set(wanted);
        known=!intel_backlight_get(&value);
    } else if(panel) {
        if(apply) {
            struct aml_object *arg=aml_integer(wanted);
            if(!arg)rc=-EMBK_ENOMEM;
            else {aml_put(aml_evaluate(method(panel,"_BCM"),&arg,1));aml_put(arg);}
        }
        uint64_t percent;
        if(aml_eval_integer(panel,"_BQC",&percent) && percent<=100) {
            for(unsigned i=0;i<cache.level_count;i++)
                if(cache.levels[i]==percent){value=percent;known=1;break;}
        }
        /* Void AML methods are valid. Verify the resulting brightness. */
        if(apply && !rc && (!known || value!=wanted))rc=-EMBK_EIO;
    }
    spin_lock(&lock);
    cache.flags&=~EMBK_DISPLAY_KNOWN;
    if(known){cache.percent=value;cache.flags|=EMBK_DISPLAY_KNOWN;}
    cache.sampled_ms=timer_uptime_ms();
    if(apply)cache.last_error=rc;
    spin_unlock(&lock);
    if(apply)__atomic_store_n(&pending,0,__ATOMIC_RELEASE);
}
int acpi_display_snapshot(struct embk_display_info *out) {
    spin_lock(&lock);*out=cache;spin_unlock(&lock);
    if(__atomic_load_n(&pending,__ATOMIC_ACQUIRE))out->flags|=EMBK_DISPLAY_BUSY;
    return 0;
}
int acpi_display_request(unsigned percent) {
    if(percent>100)return -EMBK_EINVAL;
    if(!panel && !native)return -EMBK_ENODEV;
    spin_lock(&lock);
    if(pending){spin_unlock(&lock);return -EMBK_EBUSY;}
    unsigned best=cache.levels[0],distance=101;
    for(unsigned i=0;i<cache.level_count;i++) {
        unsigned level=cache.levels[i],d=level>percent?level-percent:percent-level;
        if(d<distance){distance=d;best=level;}
    }
    __atomic_store_n(&target,best,__ATOMIC_RELAXED);
    __atomic_store_n(&pending,1,__ATOMIC_RELEASE);
    spin_unlock(&lock);return 0;
}
