#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "power/policy.c"
static uint64_t now;
static unsigned locks,notices,brightness=60,busy,hwcpus=1,profile;
static struct embk_power_info power;
void spin_lock(spinlock_t *p){assert(!p->locked);p->locked=1;}
void spin_unlock(spinlock_t *p){assert(p->locked);p->locked=0;}
uint64_t timer_uptime_ms(void){return now;}
bool boot_recovery_mode(void){return false;}
uint32_t session_active_epoch(void){return 1;}
void session_request_lock(void){locks++;}
unsigned cpu_power_count(void){return hwcpus;}
void cpu_power_profile(unsigned p){profile=p;}
int acpi_power_snapshot(struct embk_power_info *p){*p=power;return 0;}
int acpi_display_snapshot(struct embk_display_info *p){*p=(struct embk_display_info){.flags=EMBK_DISPLAY_BACKLIGHT|EMBK_DISPLAY_KNOWN|(busy?EMBK_DISPLAY_BUSY:0),.percent=brightness,.sampled_ms=now};return 0;}
int acpi_display_request(unsigned p){if(busy)return -EMBK_EBUSY;brightness=p;return 0;}
void notify_system(unsigned severity,const char *a,const char *b){assert(severity==1&&a&&b);notices++;}
static void poll(uint64_t time){now=time;power.sampled_ms=now;power_policy_poll();}
int main(void){
    struct embk_policy p={.version=1,.flags=EMBK_POLICY_LID_LOCK,.idle_dim_seconds=10,.idle_lock_seconds=20,.profile=1};
    assert(!power_policy_set(&p)&&profile==1);poll(9999);assert(brightness==60&&!locks);
    poll(10100);assert(brightness==20&&dimmed);power_activity();poll(10200);assert(brightness==60&&!dimmed);
    poll(30500);assert(locks);unsigned old=locks;
    power.flags=EMBK_POWER_LID_KNOWN|EMBK_POWER_LID_CLOSED;power_activity();poll(30600);assert(locks==old+1);
    poll(30700);assert(locks==old+1);
    power.flags=EMBK_POWER_BATTERY|EMBK_POWER_DISCHARGING;power.percent=10;poll(30800);assert(notices==1);
    poll(30900);assert(notices==1);power.percent=3;poll(31000);assert(notices==2);
    power.percent=EMBK_POWER_UNKNOWN;poll(31100);assert(notices==2);
    p.idle_dim_seconds=5;assert(power_policy_set(&p)==-EMBK_EINVAL);p.idle_dim_seconds=10;
    hwcpus=0;assert(power_policy_set(&p)==-EMBK_ENOTSUP);p.profile=0;assert(!power_policy_set(&p));
    puts("power policy: idle dim/restore, idle/lid lock, battery alerts, timing bounds and unsupported HWP OK");
}
