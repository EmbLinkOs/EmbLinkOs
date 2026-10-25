/* Exercise the real controller with a fallible scanout and a controlled clock:
 * one output through select(), and several through a layout -- hotplug,
 * arrangement trials, refusals, mirroring and the one-monitor view of them. */
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
/* Every prepare is a distinct surface, as the real allocator's are. */
static struct fb_surface initial={1280,800},pool[16],*candidate,*active=&initial;
static unsigned pool_next;
static fb_info_t mode={.width=1280,.height=800,.bpp=32};
static uint64_t clock_ms,caps=EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER);
static unsigned epoch=7,gate,alive=1,healthy=1,recovery,prepare_fail,select_fail,release_count;
static struct boot_protocol bp;
static void gpu_describe(struct embk_monitor_info *i){i->flags|=EMBK_MONITOR_MODESET;}
static const struct gpu_driver gpu={.name="test",.describe=gpu_describe};
/* Two outputs, each plugged in or not. */
static struct gpu_output heads[2];
static int plugged_changed;
static struct gpu_layout last_gl;
static struct layout_rect last_rects[4];
static unsigned last_n;
static unsigned m_count(void){return 2;}
static bool m_info(unsigned i,struct gpu_output *o){if(i>=2)return false;*o=heads[i];return true;}
static bool m_apply(const struct gpu_target *t,const struct gpu_layout *l){(void)t;(void)l;return true;}
static int m_poll(void){int c=plugged_changed;plugged_changed=0;return c;}
static const struct gpu_driver multi_gpu={.name="multi",.describe=gpu_describe,.output_count=m_count,.output_info=m_info,
                                          .apply_layout=m_apply,.poll_outputs=m_poll};
static const struct gpu_driver one_pipe={.name="one pipe",.describe=gpu_describe,.output_count=m_count,.output_info=m_info,
                                         .apply_layout=m_apply,.poll_outputs=m_poll,.max_lit=1};
static const struct gpu_driver *driver=&gpu;
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
const struct gpu_driver *gpu_active(void){return driver;}
bool fb_can_change_mode(void){return true;}
struct fb_surface *fb_prepare_mode(uint32_t w,uint32_t h){
    if(prepare_fail)return NULL;
    candidate=&pool[pool_next++%16];assert(candidate!=active);*candidate=(struct fb_surface){w,h};return candidate;
}
struct fb_surface *compositor_select_display(struct fb_surface *s){
    assert(s);if(select_fail)return NULL;struct fb_surface *old=active;active=s;
    mode.width=s->width;mode.height=s->height;return old;
}
struct fb_surface *compositor_select_layout(struct fb_surface *s,const struct gpu_layout *gl,const struct layout_rect *r,unsigned n){
    assert(s && n>=1 && n<=4);if(select_fail)return NULL;struct fb_surface *old=active;active=s;
    mode.width=s->width;mode.height=s->height;last_gl=*gl;memcpy(last_rects,r,n*sizeof *r);last_n=n;return old;
}
void fb_release_surface(struct fb_surface *s){assert(s && s!=active);release_count++;}
void kprintf(const char *fmt,...){(void)fmt;}
static struct embk_monitor_info snapshot(void){struct embk_monitor_info i;assert(!monitor_control(EMBK_MONITOR_GET,&i,sizeof i));return i;}
static int apply(unsigned w,unsigned h){struct embk_monitor_request r={1,snapshot().generation,w,h,0};return monitor_control(EMBK_MONITOR_APPLY,&r,sizeof r);}
static int action(unsigned op,uint64_t token){struct embk_monitor_request r={.version=1,.token=token};return monitor_control(op,&r,sizeof r);}
static void trial(void){assert(!apply(1024,768));assert(snapshot().phase==EMBK_MONITOR_QUEUED);monitor_tick();assert(snapshot().phase==EMBK_MONITOR_TRIAL && mode.width==1024);}
static void reset(void){memset(&state,0,sizeof state);state.generation=1;active=&initial;driver=&gpu;mode.width=1280;mode.height=800;clock_ms=0;gate=0;alive=1;healthy=1;prepare_fail=0;select_fail=0;recovery=0;}
/* ---- several outputs ------------------------------------------------------ */
static uint64_t epoch_ms=1000000;
static void head(unsigned i,const char *name,const char *model,int connected,const unsigned (*modes)[2],unsigned n){
    struct gpu_output *o=&heads[i];memset(o,0,sizeof *o);
    strcpy(o->name,name);strcpy(o->model,model);o->connected=(uint8_t)connected;o->has_edid=model[0]!=0;
    for(unsigned k=0;k<n;k++)o->modes[o->mode_count++]=(struct gpu_output_mode){(uint16_t)modes[k][0],(uint16_t)modes[k][1],60000,k==0};
}
static const unsigned modes0[][2]={{1280,800},{1024,768},{800,600}},modes1[][2]={{1024,768},{1280,800},{800,600}},wide[][2]={{1920,1080}};
static void reset_multi(int second){
    reset();driver=&multi_gpu;
    memset(&shown,0,sizeof shown);memset(&before_trial,0,sizeof before_trial);memset(&requested,0,sizeof requested);
    memset(seen_connected,0,sizeof seen_connected);memset(info_of,0,sizeof info_of);
    outputs_n=0;have_layout=layout_trial=layout_queued=0;
    head(0,"Virtual-1","",1,modes0,3);head(1,"Virtual-2","QEMU Monitor",second,modes1,3);
    /* monitor_tick polls at most once a second, by a clock that only goes forward. */
    epoch_ms+=1000000;clock_ms=epoch_ms;plugged_changed=1;
}
static void poll_tick(void){clock_ms+=1000;monitor_tick();}
static struct embk_display_layout layout_now(void){struct embk_display_layout d;assert(!monitor_control(EMBK_MONITOR_LAYOUT_GET,&d,sizeof d));return d;}
static int layout_apply(const struct embk_layout_request *r){return monitor_control(EMBK_MONITOR_LAYOUT_APPLY,(void *)r,sizeof *r);}
static struct embk_layout_request req(unsigned generation,int mirror){
    struct embk_layout_request r;memset(&r,0,sizeof r);r.version=EMBK_DISPLAY_LAYOUT_VERSION;r.generation=generation;r.count=2;
    r.flags=mirror?EMBK_LAYOUT_MIRROR:0;return r;
}
static void out(struct embk_layout_request *r,unsigned i,int primary,unsigned w,unsigned h,int x,int y){
    r->out[i].enabled=1;r->out[i].primary=(uint32_t)primary;r->out[i].width=w;r->out[i].height=h;r->out[i].x=x;r->out[i].y=y;
}
static int rect_is(unsigned i,int x,int y,unsigned w,unsigned h){
    return i<last_n && last_rects[i].x==x && last_rects[i].y==y && last_rects[i].w==w && last_rects[i].h==h;
}
static void multi_outputs(void){
    /* Boot with a second monitor: it joins on the right at its preferred mode, at once. */
    reset_multi(1);
    unsigned released=release_count;
    poll_tick();
    struct embk_display_layout d=layout_now();
    assert((d.flags&EMBK_LAYOUT_MULTI) && !(d.flags&EMBK_LAYOUT_MIRROR) && d.count==2 && d.phase==EMBK_MONITOR_IDLE);
    assert(mode.width==2304 && mode.height==800 && d.desktop_width==2304 && d.desktop_height==800);
    assert(d.outputs[0].flags==(EMBK_OUTPUT_CONNECTED|EMBK_OUTPUT_ENABLED|EMBK_OUTPUT_PRIMARY) && d.outputs[0].width==1280);
    assert(d.outputs[1].flags==(EMBK_OUTPUT_CONNECTED|EMBK_OUTPUT_ENABLED|EMBK_OUTPUT_EDID) && d.outputs[1].x==1280 && d.outputs[1].y==0);
    assert(!strcmp(d.outputs[1].name,"Virtual-2") && !strcmp(d.outputs[1].model,"QEMU Monitor") && d.outputs[1].mode_count==3);
    assert(d.outputs[1].modes[0].flags==(EMBK_MONITOR_PREFERRED|EMBK_MONITOR_CURRENT) && d.outputs[1].refresh_millihz==60000);
    assert(last_n==2 && rect_is(0,0,0,1280,800) && rect_is(1,1280,0,1024,768) && last_gl.count==2 && last_gl.out[1].x==1280);
    assert(last_gl.primary==0);
    assert(release_count==released+1 && active!=&initial);
    /* The one-monitor view is the primary: its size and its own modes. */
    struct embk_monitor_info info=snapshot();
    assert(info.width==1280 && info.height==800 && info.mode_count==3 && info.modes[0].width==1280);
    assert((info.modes[0].flags&EMBK_MONITOR_CURRENT) && (info.modes[0].flags&EMBK_MONITOR_PREFERRED));
    /* Nothing changed: no poll result, no new layout. */
    unsigned generation=d.generation;poll_tick();assert(layout_now().generation==generation);

    /* Refusals. */
    struct embk_layout_request r=req(d.generation,0);
    out(&r,0,1,1280,800,1024,0);out(&r,1,0,1024,768,0,0);
    struct embk_layout_request bad=r;bad.generation--;assert(layout_apply(&bad)==-EMBK_EAGAIN);
    bad=r;bad.count=3;assert(layout_apply(&bad)==-EMBK_EINVAL);
    bad=r;bad.version=2;assert(layout_apply(&bad)==-EMBK_EINVAL);
    bad=r;bad.out[1].width=1000;assert(layout_apply(&bad)==-EMBK_EINVAL);                /* no such mode */
    bad=r;bad.out[1].x=1100;assert(layout_apply(&bad)==-EMBK_EINVAL);                    /* overlap */
    bad=r;bad.out[0].x=1200;assert(layout_apply(&bad)==-EMBK_EINVAL);                    /* a gap */
    bad=r;bad.out[1].primary=1;assert(layout_apply(&bad)==-EMBK_EINVAL);                 /* two primaries */
    bad=req(d.generation,0);out(&bad,0,1,1280,800,0,0);out(&bad,1,0,1024,768,1280,0);
    assert(layout_apply(&bad)==-EMBK_EINVAL);                                             /* what is shown already */
    bad=req(d.generation,1);out(&bad,0,1,1280,800,0,0);out(&bad,1,0,1024,768,0,0);
    assert(layout_apply(&bad)==-EMBK_EINVAL);                                             /* mirrored at two sizes */
    caps=0;assert(layout_apply(&r)==-EMBK_EPERM);caps=EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER);

    /* The monitor to the left: a trial, kept. */
    assert(!layout_apply(&r) && layout_now().phase==EMBK_MONITOR_QUEUED);
    assert(layout_apply(&r)==-EMBK_EBUSY);
    monitor_tick();
    d=layout_now();
    assert(d.phase==EMBK_MONITOR_TRIAL && d.outputs[0].x==1024 && d.outputs[1].x==0 && d.remaining_ms==30000);
    assert(rect_is(0,1024,0,1280,800) && rect_is(1,0,0,1024,768) && mode.width==2304);
    assert(snapshot().flags&EMBK_MONITOR_OWN_TRIAL);
    /* A monitor pulled out during the trial waits for it. */
    heads[1].connected=0;plugged_changed=1;poll_tick();
    assert(layout_now().outputs[1].flags&EMBK_OUTPUT_ENABLED);
    released=release_count;
    assert(!action(EMBK_MONITOR_KEEP,d.token) && release_count==released+1 && layout_now().phase==EMBK_MONITOR_IDLE);
    /* ...and is acted on after it. The laptop panel slides to the origin. */
    poll_tick();
    d=layout_now();
    assert(!(d.outputs[1].flags&(EMBK_OUTPUT_CONNECTED|EMBK_OUTPUT_ENABLED)) && d.outputs[0].x==0 && mode.width==1280 && mode.height==800);
    assert(last_n==1 && rect_is(0,0,0,1280,800));
    /* Plugged back in: on the right again, the old arrangement is the desktop's to restore. */
    heads[1].connected=1;plugged_changed=1;poll_tick();
    d=layout_now();assert(d.outputs[1].x==1280 && mode.width==2304);

    /* A trial reverted on request, and one that times out. */
    r=req(d.generation,0);out(&r,0,1,1280,800,0,0);out(&r,1,0,1024,768,0,800);
    assert(!layout_apply(&r));monitor_tick();
    d=layout_now();assert(d.phase==EMBK_MONITOR_TRIAL && d.outputs[1].y==800 && mode.width==1280 && mode.height==1568);
    assert(!action(EMBK_MONITOR_REVERT,d.token));monitor_tick();
    d=layout_now();assert(d.phase==EMBK_MONITOR_IDLE && d.outputs[1].x==1280 && d.outputs[1].y==0 && mode.width==2304);
    assert(rect_is(1,1280,0,1024,768));
    r.generation=d.generation;assert(!layout_apply(&r));monitor_tick();
    clock_ms+=30000;monitor_tick();
    d=layout_now();assert(d.phase==EMBK_MONITOR_IDLE && d.outputs[1].x==1280 && mode.width==2304 && mode.height==800);

    /* A resolution change from the one-monitor interface: the primary
     * shrinks and the monitor on its right follows its edge. */
    assert(!apply(1024,768));monitor_tick();
    d=layout_now();assert(d.phase==EMBK_MONITOR_TRIAL && d.outputs[0].width==1024 && d.outputs[1].x==1024 && mode.width==2048 && mode.height==768);
    info=snapshot();assert(info.width==1024 && info.height==768);
    assert(!action(EMBK_MONITOR_KEEP,d.token));

    /* The primary's monitor goes away: the other takes the role and the origin. */
    heads[0].connected=0;plugged_changed=1;poll_tick();
    d=layout_now();
    assert(!(d.outputs[0].flags&EMBK_OUTPUT_ENABLED) && (d.outputs[1].flags&EMBK_OUTPUT_PRIMARY) && d.outputs[1].x==0);
    assert(mode.width==1024 && mode.height==768 && snapshot().width==1024 && !strcmp(snapshot().name,"QEMU Monitor"));
    assert(last_gl.primary==1 && last_n==1 && rect_is(0,0,0,1024,768));

    /* Mirroring, and monitors joining a mirror. */
    reset_multi(1);poll_tick();d=layout_now();
    r=req(d.generation,1);out(&r,0,1,1024,768,0,0);out(&r,1,0,1024,768,0,0);
    assert(!layout_apply(&r));monitor_tick();d=layout_now();
    assert((d.flags&EMBK_LAYOUT_MIRROR) && mode.width==1024 && mode.height==768 && rect_is(0,0,0,1024,768) && rect_is(1,0,0,1024,768));
    assert(!action(EMBK_MONITOR_KEEP,d.token));
    heads[1].connected=0;plugged_changed=1;poll_tick();
    heads[1].connected=1;plugged_changed=1;poll_tick();
    d=layout_now();assert((d.flags&EMBK_LAYOUT_MIRROR) && (d.outputs[1].flags&EMBK_OUTPUT_ENABLED) && d.outputs[1].x==0 && d.outputs[1].width==1024);
    /* One that cannot show the mirrored size waits, unused, for Settings. */
    heads[1].connected=0;plugged_changed=1;poll_tick();
    head(1,"Virtual-2","Wide",1,wide,1);plugged_changed=1;poll_tick();
    d=layout_now();assert((d.flags&EMBK_LAYOUT_MIRROR) && (d.outputs[1].flags&EMBK_OUTPUT_CONNECTED) && !(d.outputs[1].flags&EMBK_OUTPUT_ENABLED));
    assert(mode.width==1024 && last_n==1);

    /* An adapter that lights one output at a time: the second monitor waits,
     * unused, and a request for both is refused. */
    reset_multi(1);driver=&one_pipe;poll_tick();d=layout_now();
    assert(d.max_enabled==1 && (d.outputs[1].flags&EMBK_OUTPUT_CONNECTED) && !(d.outputs[1].flags&EMBK_OUTPUT_ENABLED) && mode.width==1280);
    r=req(d.generation,0);out(&r,0,1,1280,800,0,0);out(&r,1,0,1024,768,1280,0);
    assert(layout_apply(&r)==-EMBK_EINVAL);
    r=req(d.generation,0);out(&r,1,1,1024,768,0,0);                       /* the other one instead: fine */
    assert(!layout_apply(&r));monitor_tick();assert(mode.width==1024 && layout_now().phase==EMBK_MONITOR_TRIAL);
    assert(!action(EMBK_MONITOR_KEEP,layout_now().token));

    /* A layout the device refuses leaves the old one on screen. */
    reset_multi(1);select_fail=1;poll_tick();d=layout_now();
    assert(mode.width==1280 && !(d.outputs[1].flags&EMBK_OUTPUT_ENABLED));
    select_fail=0;
}

int main(void){
    reset();caps=0;assert(apply(1024,768)==-EMBK_EPERM);caps=EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER);
    recovery=1;assert(apply(1024,768)==-EMBK_EPERM);recovery=0;healthy=0;assert(apply(1024,768)==-EMBK_EPERM);healthy=1;
    gate=caller.pid;assert(apply(1024,768)==-EMBK_EPERM);gate=0;
    assert(apply(777,555)==-EMBK_EINVAL && apply(1280,800)==-EMBK_EINVAL);
    struct embk_monitor_request stale={1,0,1024,768,0};assert(monitor_control(EMBK_MONITOR_APPLY,&stale,sizeof stale)==-EMBK_EAGAIN);
    trial();uint64_t token=snapshot().token;assert(snapshot().flags&EMBK_MONITOR_OWN_TRIAL);
    assert(action(EMBK_MONITOR_KEEP,token+1)==-EMBK_EPERM);assert(apply(800,600)==-EMBK_EBUSY);
    unsigned released=release_count;assert(!action(EMBK_MONITOR_KEEP,token));assert(active==candidate && release_count==released+1);
    reset();trial();token=snapshot().token;assert(!action(EMBK_MONITOR_REVERT,token));monitor_tick();assert(active==&initial && snapshot().phase==EMBK_MONITOR_IDLE);
    reset();trial();clock_ms=29999;monitor_tick();assert(active==candidate);clock_ms=30000;monitor_tick();assert(active==&initial);
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
    multi_outputs();
    puts("Display controller, several outputs: hotplug extends and shrinks the desktop, the primary moves with its "
         "monitor, arrangement trials keep/revert/time out, refusals, hotplug waits for a trial, mirror joins, the "
         "one-monitor view is the primary OK");
}
