#include "power/policy.h"
#include "boot/boot_protocol.h"
#include "acpi/power.h"
#include "acpi/display.h"
#include "process/session.h"
#include "ipc/notify.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "include/errno.h"
static spinlock_t lock=SPINLOCK_INIT;
static struct embk_policy config={.version=EMBK_POLICY_VERSION,.flags=EMBK_POLICY_LID_LOCK,.idle_dim_seconds=120,.idle_lock_seconds=300};
static uint64_t activity,next_poll;
static unsigned dimmed,saved_percent,was_closed,low_level,poll_busy;
void power_activity(void) {__atomic_store_n(&activity,timer_uptime_ms(),__ATOMIC_RELEASE);}
void power_policy_get(struct embk_policy *out) {
    spin_lock(&lock);*out=config;spin_unlock(&lock);
    out->last_activity_ms=__atomic_load_n(&activity,__ATOMIC_ACQUIRE);
    out->hwp_cpus=cpu_power_count();if(out->hwp_cpus)out->flags|=EMBK_POLICY_HWP;
    if(__atomic_load_n(&dimmed,__ATOMIC_ACQUIRE))out->flags|=EMBK_POLICY_DIMMED;
}
int power_policy_set(const struct embk_policy *p) {
    if(p->version!=EMBK_POLICY_VERSION || (p->flags&~EMBK_POLICY_LID_LOCK) || p->profile>2 ||
       p->idle_dim_seconds>86400 || p->idle_lock_seconds>86400 ||
       (p->idle_dim_seconds && p->idle_dim_seconds<10) || (p->idle_lock_seconds && p->idle_lock_seconds<10))return -EMBK_EINVAL;
    if(p->profile && !cpu_power_count())return -EMBK_ENOTSUP;
    spin_lock(&lock);config=*p;config.last_activity_ms=0;config.hwp_cpus=0;spin_unlock(&lock);
    cpu_power_profile(p->profile);power_activity();return 0;
}
static void poll_once(void) {
    if(boot_recovery_mode())return;
    uint64_t now=timer_uptime_ms();if(now<next_poll)return;next_poll=now+100;
    struct embk_policy p;power_policy_get(&p);
    uint64_t idle=now>=p.last_activity_ms?now-p.last_activity_ms:0;
    if(p.idle_lock_seconds && idle>=(uint64_t)p.idle_lock_seconds*1000)session_request_lock();
    struct embk_power_info battery;
    if(!acpi_power_snapshot(&battery) && now-battery.sampled_ms<=10000) {
        unsigned closed=(battery.flags&(EMBK_POWER_LID_KNOWN|EMBK_POWER_LID_CLOSED))==(EMBK_POWER_LID_KNOWN|EMBK_POWER_LID_CLOSED);
        if(closed && !was_closed && (p.flags&EMBK_POLICY_LID_LOCK))session_request_lock();
        was_closed=closed;
        unsigned level=(battery.flags&EMBK_POWER_DISCHARGING) && battery.percent!=EMBK_POWER_UNKNOWN?
            battery.percent<=3?2:battery.percent<=10?1:0:0;
        if(!session_active_epoch())low_level=0;
        if(session_active_epoch() && level>low_level)notify_system(1,"Battery",level==2?"Battery is critically low. Save your work and connect power.":"Battery is low. Connect power soon.");
        if(session_active_epoch())low_level=level;
    }
    struct embk_display_info display;
    if(acpi_display_snapshot(&display) || !(display.flags&EMBK_DISPLAY_KNOWN) ||
       (display.flags&EMBK_DISPLAY_BUSY) || now-display.sampled_ms>10000)return;
    int should_dim=p.idle_dim_seconds && idle>=(uint64_t)p.idle_dim_seconds*1000;
    if(should_dim && !dimmed && display.percent>20) {
        saved_percent=display.percent;
        if(!acpi_display_request(20))__atomic_store_n(&dimmed,1,__ATOMIC_RELEASE);
    } else if(!should_dim && dimmed) {
        if(!acpi_display_request(saved_percent))__atomic_store_n(&dimmed,0,__ATOMIC_RELEASE);
    }
}

void power_policy_poll(void) {
    if(__atomic_exchange_n(&poll_busy,1,__ATOMIC_ACQ_REL))return;
    poll_once();__atomic_store_n(&poll_busy,0,__ATOMIC_RELEASE);
}
