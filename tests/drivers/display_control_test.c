/* Exercise the real controller with a fallible scanout and a controlled clock. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "process/process.h"
#undef current_thread
#undef current_process
static struct process caller={.pid=12};
static struct thread thread;
#define current_thread test_thread
static struct thread *test_thread=&thread;
#define current_process (&caller)
#include "display/control.c"
struct fb_surface {unsigned width,height;};
static struct fb_surface initial={1280,800},candidate,*active=&initial;
static fb_info_t mode={.width=1280,.height=800,.bpp=32};
static uint64_t clock_ms,caps=EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER);
static unsigned epoch=7,gate,alive=1,healthy=1,recovery,prepare_fail,select_fail,release_count;
static struct boot_protocol bp;
static void gpu_describe(struct embk_monitor_info *i){i->flags|=EMBK_MONITOR_MODESET;}
static const struct gpu_driver gpu={.name="test",.describe=gpu_describe};
void mutex_lock(struct mutex *m){assert(!m->locked);m->locked=1;}
int mutex_trylock(struct mutex *m){if(m->locked)return 0;m->locked=1;return 1;}
void mutex_unlock(struct mutex *m){assert(m->locked);m->locked=0;}
uint64_t time_get_ns(void){return clock_ms*1000000;}
uint64_t process_current_caps(void){return caps;}
int process_alive(uint32_t pid){return pid==caller.pid && alive;}
int session_ui_allowed(int pid){return !gate || gate==(unsigned)pid;}
uint32_t session_input_owner(void){return gate;}
uint32_t session_active_epoch(void){return epoch;}
bool boot_recovery_mode(void){return recovery;}
const struct boot_protocol *boot_protocol_get(void){return &bp;}
int compositor_desktop_healthy(void){return healthy;}
void compositor_display_lock(void){}
void compositor_display_unlock(void){}
const fb_info_t *fb_get_info(void){return &mode;}
const struct gpu_driver *gpu_active(void){return &gpu;}
bool fb_can_change_mode(void){return true;}
struct fb_surface *fb_prepare_mode(uint32_t w,uint32_t h){candidate=(struct fb_surface){w,h};return prepare_fail?NULL:&candidate;}
struct fb_surface *compositor_select_display(struct fb_surface *s){
    assert(s);if(select_fail)return NULL;struct fb_surface *old=active;active=s;
    mode.width=s->width;mode.height=s->height;return old;
}
void fb_release_surface(struct fb_surface *s){assert(s && s!=active);release_count++;}
void kprintf(const char *fmt,...){(void)fmt;}
static struct embk_monitor_info snapshot(void){struct embk_monitor_info i;assert(!monitor_control(EMBK_MONITOR_GET,&i,sizeof i));return i;}
static int apply(unsigned w,unsigned h){struct embk_monitor_request r={1,snapshot().generation,w,h,0};return monitor_control(EMBK_MONITOR_APPLY,&r,sizeof r);}
static int action(unsigned op,uint64_t token){struct embk_monitor_request r={.version=1,.token=token};return monitor_control(op,&r,sizeof r);}
static void trial(void){assert(!apply(1024,768));assert(snapshot().phase==EMBK_MONITOR_QUEUED);monitor_tick();assert(snapshot().phase==EMBK_MONITOR_TRIAL && mode.width==1024);}
static void reset(void){memset(&state,0,sizeof state);state.generation=1;active=&initial;mode.width=1280;mode.height=800;clock_ms=0;gate=0;alive=1;healthy=1;prepare_fail=0;select_fail=0;recovery=0;}
int main(void){
    reset();caps=0;assert(apply(1024,768)==-EMBK_EPERM);caps=EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER);
    recovery=1;assert(apply(1024,768)==-EMBK_EPERM);recovery=0;healthy=0;assert(apply(1024,768)==-EMBK_EPERM);healthy=1;
    gate=caller.pid;assert(apply(1024,768)==-EMBK_EPERM);gate=0;
    assert(apply(777,555)==-EMBK_EINVAL && apply(1280,800)==-EMBK_EINVAL);
    struct embk_monitor_request stale={1,0,1024,768,0};assert(monitor_control(EMBK_MONITOR_APPLY,&stale,sizeof stale)==-EMBK_EAGAIN);
    trial();uint64_t token=snapshot().token;assert(snapshot().flags&EMBK_MONITOR_OWN_TRIAL);
    assert(action(EMBK_MONITOR_KEEP,token+1)==-EMBK_EPERM);assert(apply(800,600)==-EMBK_EBUSY);
    unsigned released=release_count;assert(!action(EMBK_MONITOR_KEEP,token));assert(active==&candidate && release_count==released+1);
    reset();trial();token=snapshot().token;assert(!action(EMBK_MONITOR_REVERT,token));monitor_tick();assert(active==&initial && snapshot().phase==EMBK_MONITOR_IDLE);
    reset();trial();clock_ms=29999;monitor_tick();assert(active==&candidate);clock_ms=30000;monitor_tick();assert(active==&initial);
    reset();trial();clock_ms=30000;assert(action(EMBK_MONITOR_KEEP,snapshot().token)==-EMBK_ECANCELED);monitor_tick();assert(active==&initial);
    reset();trial();alive=0;monitor_tick();assert(active==&initial);
    reset();trial();epoch++;monitor_tick();assert(active==&initial);
    reset();trial();gate=99;monitor_tick();assert(active==&initial);
    reset();assert(!apply(1024,768));alive=0;monitor_tick();assert(active==&initial && snapshot().phase==EMBK_MONITOR_ERROR);
    reset();prepare_fail=1;assert(!apply(1024,768));monitor_tick();assert(active==&initial && snapshot().phase==EMBK_MONITOR_ERROR);
    reset();select_fail=1;assert(!apply(1024,768));released=release_count;monitor_tick();assert(active==&initial && release_count==released+1);
    reset();trial();select_fail=1;clock_ms=30000;monitor_tick();assert(snapshot().phase==EMBK_MONITOR_ERROR);
    assert(!(snapshot().flags&EMBK_MONITOR_MODESET) && apply(800,600)==-EMBK_EBUSY);
    puts("Display controller: permissions, stale requests, exact token, confirmation, cancel, timeout, process exit, session lock/exit and device failure OK");
}
