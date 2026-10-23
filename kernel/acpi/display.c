/* Standard ACPI video backlight methods. Firmware that exposes no usable
 * _BCL/_BCM/_BQC leaves the control unavailable; a framebuffer fade would not
 * reduce panel power and is deliberately not substituted for brightness. */
#include "acpi/display.h"
#include "acpi/power.h"
#include "drivers/video/intel_display.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "drivers/timer/timer.h"
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/kernel_api.h>
static spinlock_t lock=SPINLOCK_INIT;
static struct embk_display_info cache={.version=EMBK_DISPLAY_VERSION};
static uacpi_namespace_node *panel;
static uacpi_handle work;
static unsigned pending,target,native;
static uacpi_iteration_decision discover(void *ctx,uacpi_namespace_node *node,uacpi_u32 depth) {
    (void)ctx;(void)depth;uacpi_namespace_node *method;
    if(uacpi_namespace_node_find(node,"_BCM",&method)!=UACPI_STATUS_OK ||
       uacpi_namespace_node_find(node,"_BQC",&method)!=UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_object *o=NULL;
    if(uacpi_eval_simple_package(node,"_BCL",&o)!=UACPI_STATUS_OK) return UACPI_ITERATION_DECISION_CONTINUE;
    uacpi_object_array a;struct embk_display_info v={.version=EMBK_DISPLAY_VERSION};
    int valid=uacpi_object_get_package(o,&a)==UACPI_STATUS_OK && a.count>=3 && a.count<=34;
    for(unsigned i=0;valid && i<a.count;i++) {
        uint64_t value;
        if(uacpi_object_get_integer(a.objects[i],&value)!=UACPI_STATUS_OK || value>100) {valid=0;break;}
        if(i<2) continue;
        unsigned j=0;while(j<v.level_count && v.levels[j]!=value) j++;
        if(j==v.level_count) v.levels[v.level_count++]=value;
    }
    uacpi_object_unref(o);
    if(!valid || !v.level_count) return UACPI_ITERATION_DECISION_CONTINUE;
    for(unsigned i=1;i<v.level_count;i++) for(unsigned j=i;j && v.levels[j]<v.levels[j-1];j--) {
        unsigned t=v.levels[j];v.levels[j]=v.levels[j-1];v.levels[j-1]=t;
    }
    v.flags=EMBK_DISPLAY_BACKLIGHT;panel=node;cache=v;return UACPI_ITERATION_DECISION_BREAK;
}
void acpi_display_refresh(void) {
    if(native) {
        unsigned percent=0;int rc=intel_backlight_get(&percent);
        spin_lock(&lock);cache.flags&=~EMBK_DISPLAY_KNOWN;
        if(!rc) {cache.percent=percent;cache.flags|=EMBK_DISPLAY_KNOWN;}
        cache.sampled_ms=timer_uptime_ms();cache.last_error=rc;spin_unlock(&lock);return;
    }
    if(!panel) return;
    uint64_t value=0;int ok=uacpi_eval_simple_integer(panel,"_BQC",&value)==UACPI_STATUS_OK && value<=100;
    spin_lock(&lock);cache.flags&=~EMBK_DISPLAY_KNOWN;
    for(unsigned i=0;ok && i<cache.level_count;i++) if(cache.levels[i]==value) {
        cache.percent=value;cache.flags|=EMBK_DISPLAY_KNOWN;break;
    }
    cache.sampled_ms=timer_uptime_ms();spin_unlock(&lock);
}
static void apply(uacpi_handle arg) {
    (void)arg;unsigned percent=__atomic_load_n(&target,__ATOMIC_ACQUIRE);
    uacpi_object *value=uacpi_object_create_integer(percent);
    int rc=-EMBK_ENOMEM;
    if(value) {
        rc=uacpi_eval(panel,"_BCM",&(uacpi_object_array){&value,1},NULL)==UACPI_STATUS_OK?0:-EMBK_EIO;
        uacpi_object_unref(value);
    }
    acpi_display_refresh();spin_lock(&lock);cache.last_error=rc;spin_unlock(&lock);
    __atomic_store_n(&pending,0,__ATOMIC_RELEASE);
}
void acpi_display_init(void) {
    if(acpi_runtime_ready())
        uacpi_namespace_for_each_child(uacpi_namespace_root(),discover,NULL,UACPI_OBJECT_DEVICE_BIT,UACPI_MAX_DEPTH_ANY,NULL);
    if(!panel) {
        unsigned value;
        if(!intel_backlight_get(&value)) {
            native=1;cache.flags=EMBK_DISPLAY_BACKLIGHT|EMBK_DISPLAY_NATIVE;
            for(unsigned i=0;i<=100;i+=5)cache.levels[cache.level_count++]=i;
            acpi_display_refresh();
        }
    }
    if(panel) {work=uacpi_kernel_create_work_item();acpi_display_refresh();}
}
int acpi_display_snapshot(struct embk_display_info *out) {
    if(native)acpi_display_refresh();
    spin_lock(&lock);*out=cache;spin_unlock(&lock);
    if(__atomic_load_n(&pending,__ATOMIC_ACQUIRE)) out->flags|=EMBK_DISPLAY_BUSY;
    return 0;
}
int acpi_display_request(unsigned percent) {
    if(percent>100) return -EMBK_EINVAL;
    if(native) {int rc=intel_backlight_set(percent);acpi_display_refresh();return rc;}
    if(!panel || !work) return -EMBK_ENODEV;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&pending,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return -EMBK_EBUSY;
    unsigned best=cache.levels[0],distance=101;
    for(unsigned i=0;i<cache.level_count;i++) {
        unsigned level=cache.levels[i],d=level>percent?level-percent:percent-level;
        if(d<distance) {distance=d;best=level;}
    }
    __atomic_store_n(&target,best,__ATOMIC_RELEASE);
    uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION,work,apply,NULL);
    return 0;
}
