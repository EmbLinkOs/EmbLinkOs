#include "control.h"
#include "edid.h"
#include "layout.h"
#include "drivers/video/gpu.h"
#include "drivers/video/framebuffer.h"
#include "gfx/compositor.h"
#include "process/process.h"
#include "process/capabilities.h"
#include "process/ksync.h"
#include "process/session.h"
#include "boot/boot_protocol.h"
#include "drivers/timer/timer.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "include/errno.h"

/* ARM's screen-session controller is not ported yet; match session.h's
 * existing permissive screen-ownership fallback without a missing symbol. */
static uint32_t monitor_session_epoch(void) {
#if defined(__x86_64__)
    return session_active_epoch();
#else
    return 0;
#endif
}

static struct mutex lock=MUTEX_INIT;
static struct {unsigned phase,generation,owner,epoch,width,height;uint64_t token,deadline;int error;struct fb_surface *previous;} state={.generation=1};
static uint64_t next_token=1;
static const unsigned standard[][2]={{640,480},{800,600},{1024,768},{1280,720},{1280,800},{1280,1024},{1440,900},{1600,900},{1680,1050},{1920,1080},{1920,1200},{2560,1440},{3840,2160}};

/* ---- several monitors ---------------------------------------------------------
 * With a driver that has several outputs, what is on screen is a layout: one
 * surface and the rectangle each output shows (display/layout.h). It changes
 * three ways -- a monitor plugged in or out (at once, no trial: nobody chose
 * it), Settings arranging the monitors, or a resolution change -- and the last
 * two run the same trial a resolution change always has. */
static struct gpu_output info_of[GPU_MAX_OUTPUTS];
static unsigned outputs_n;                 /* 0: a one-output driver */
static struct layout shown, before_trial, requested;
static uint8_t seen_connected[GPU_MAX_OUTPUTS];
static int have_layout, layout_trial, layout_queued;

static int multi(void) {
    const struct gpu_driver *g=gpu_active();
    return g && g->output_count && g->output_info && g->apply_layout && g->poll_outputs;
}

static void read_outputs(void) {
    const struct gpu_driver *g=gpu_active();
    outputs_n=g->output_count();
    if(outputs_n>GPU_MAX_OUTPUTS)outputs_n=GPU_MAX_OUTPUTS;
    for(unsigned i=0;i<outputs_n;i++)if(!g->output_info(i,&info_of[i]))memset(&info_of[i],0,sizeof info_of[i]);
}

/* The layout the boot left: output 0 showing the whole framebuffer. */
static void initial_layout(void) {
    memset(&shown,0,sizeof shown);
    shown.count=outputs_n;
    const fb_info_t *fb=fb_get_info();
    for(unsigned i=0;i<outputs_n;i++)shown.out[i].connected=info_of[i].connected;
    shown.out[0].connected=1;shown.out[0].enabled=1;shown.out[0].primary=1;
    shown.out[0].w=fb?fb->width:1024;shown.out[0].h=fb?fb->height:768;
    have_layout=1;
}

static void gpu_layout_of(const struct layout *l,struct gpu_layout *g) {
    memset(g,0,sizeof *g);
    g->count=l->count;
    for(unsigned i=0;i<l->count;i++) {
        g->out[i]=(struct gpu_output_cfg){l->out[i].enabled,l->out[i].x,l->out[i].y,l->out[i].w,l->out[i].h};
        if(l->out[i].enabled && l->out[i].primary)g->primary=i;
    }
}

/* Show `l`. On success *previous is the surface that was on screen, still
 * allocated: released by the caller now, or after a trial. */
static int show_layout(struct layout *l,struct fb_surface **previous) {
    layout_normalize(l);
    uint32_t w,h;
    int v=layout_validate(l,&w,&h);
    if(v!=LAYOUT_OK)return -EMBK_EINVAL;
    struct fb_surface *next=fb_prepare_mode(w,h);
    if(!next)return -EMBK_ENOMEM;
    struct gpu_layout gl;gpu_layout_of(l,&gl);
    struct layout_rect rects[LAYOUT_MAX_OUTPUTS];
    unsigned n=layout_rects(l,rects,LAYOUT_MAX_OUTPUTS);
    struct fb_surface *old=compositor_select_layout(next,&gl,rects,n);
    if(!old){fb_release_surface(next);return -EMBK_EIO;}
    *previous=old;
    return 0;
}

static int same_layout(const struct layout *a,const struct layout *b) {
    if(a->count!=b->count || a->mirror!=b->mirror)return 0;
    for(unsigned i=0;i<a->count;i++) {
        const struct layout_out *x=&a->out[i],*y=&b->out[i];
        if(x->enabled!=y->enabled || (x->enabled && (x->primary!=y->primary || x->x!=y->x || x->y!=y->y || x->w!=y->w || x->h!=y->h)))return 0;
    }
    return 1;
}

static unsigned max_lit(void) {
    const struct gpu_driver *g=gpu_active();
    return g && g->max_lit ? g->max_lit : GPU_MAX_OUTPUTS;
}

static unsigned lit(const struct layout *l) {
    unsigned n=0;
    for(unsigned i=0;i<l->count;i++)n+=l->out[i].enabled?1u:0u;
    return n;
}

static int has_mode(unsigned i,uint32_t w,uint32_t h) {
    for(unsigned k=0;k<info_of[i].mode_count;k++)if(info_of[i].modes[k].width==w && info_of[i].modes[k].height==h)return 1;
    return 0;
}

/* The size every enabled output of a mirror shows. */
static int mirror_size(const struct layout *l,uint32_t *w,uint32_t *h) {
    for(unsigned i=0;i<l->count;i++)if(l->out[i].enabled){*w=l->out[i].w;*h=l->out[i].h;return 1;}
    return 0;
}

/* A monitor plugged in joins on the right at its preferred mode -- or, when
 * the others are mirrored, joins the mirror if it can show that size, and
 * otherwise waits, connected but unused, for Settings: the mirror is a choice
 * someone made and a new cable is not a reason to undo it. One pulled out
 * leaves, and the primary moves if it was the one. */
static void hotplug(void) {
    read_outputs();
    if(!have_layout)initial_layout();
    struct layout next=shown;
    next.count=outputs_n;
    for(unsigned i=0;i<outputs_n;i++) {
        struct layout_out *o=&next.out[i];
        int connected=info_of[i].connected;
        o->connected=(uint8_t)connected;
        if(!connected){o->enabled=0;o->primary=0;}
        else if(!seen_connected[i] && !o->enabled && info_of[i].mode_count) {
            uint32_t mw=0,mh=0;
            if(lit(&next)>=max_lit())
                kprintf("display: %s connected -- %u displays are already in use, the most this adapter shows; not in use\n",
                        info_of[i].name,lit(&next));
            else if(!next.mirror) {
                o->enabled=1;o->primary=0;
                o->w=info_of[i].modes[0].width;o->h=info_of[i].modes[0].height;
                layout_place_right(&next,i);
                kprintf("display: %s connected -- %ux%u to the right\n",info_of[i].name,o->w,o->h);
            }else if(mirror_size(&next,&mw,&mh) && has_mode(i,mw,mh)) {
                o->enabled=1;o->primary=0;o->w=mw;o->h=mh;o->x=o->y=0;
                kprintf("display: %s connected -- mirroring at %ux%u\n",info_of[i].name,mw,mh);
            }else kprintf("display: %s connected -- cannot show the mirrored %ux%u; not in use\n",info_of[i].name,mw,mh);
        }
        if(seen_connected[i] && !connected)kprintf("display: %s disconnected\n",info_of[i].name);
        seen_connected[i]=(uint8_t)connected;
    }
    unsigned primaries=0;
    for(unsigned i=0;i<outputs_n;i++)if(next.out[i].enabled && next.out[i].primary)primaries++;
    if(!primaries)for(unsigned i=0;i<outputs_n;i++)if(next.out[i].enabled){next.out[i].primary=1;break;}
    if(same_layout(&next,&shown))return;
    struct fb_surface *old;
    if(show_layout(&next,&old)) {
        kprintf("display: the new arrangement could not be shown; keeping the old one\n");
        return;
    }
    fb_release_surface(old);
    shown=next;state.generation++;
    struct layout_rect rects[LAYOUT_MAX_OUTPUTS];
    kprintf("display: desktop is now %ux%u over %u output(s)\n",fb_get_info()->width,fb_get_info()->height,
            layout_rects(&shown,rects,LAYOUT_MAX_OUTPUTS));
}

static int primary_index(const struct layout *l) {
    for(unsigned i=0;i<l->count;i++)if(l->out[i].enabled && l->out[i].primary)return (int)i;
    return 0;
}

/* A resolution change from the one-monitor interface, on a layout: the
 * primary changes size, and outputs right of or below it move with its edge. */
static struct layout resized_primary(unsigned w,unsigned h) {
    struct layout l=shown;
    int p=primary_index(&l);
    int32_t old_right=l.out[p].x+(int32_t)l.out[p].w,old_bottom=l.out[p].y+(int32_t)l.out[p].h;
    int32_t dx=(int32_t)w-(int32_t)l.out[p].w,dy=(int32_t)h-(int32_t)l.out[p].h;
    for(unsigned i=0;i<l.count;i++) {
        if((int)i==p || !l.out[i].enabled)continue;
        if(l.out[i].x>=old_right)l.out[i].x+=dx;
        if(l.out[i].y>=old_bottom)l.out[i].y+=dy;
    }
    l.out[p].w=w;l.out[p].h=h;
    return l;
}

static void describe(struct embk_monitor_info *info) {
    memset(info,0,sizeof *info);info->version=EMBK_MONITOR_VERSION;
    compositor_display_lock();const fb_info_t *fb=fb_get_info();
    if(fb){info->width=fb->width;info->height=fb->height;info->flags=EMBK_MONITOR_CONNECTED;}
    compositor_display_unlock();
    const struct gpu_driver *gpu=gpu_active();
    strncpy(info->driver,gpu?gpu->name:"Firmware framebuffer",sizeof info->driver-1);
    strcpy(info->name,gpu && gpu->prepare?"Virtual display":"Firmware display");
    for(unsigned i=0;i<sizeof standard/sizeof standard[0];i++) {
        struct embk_monitor_mode *m=&info->modes[info->mode_count++];m->width=standard[i][0];m->height=standard[i][1];
    }
    const struct boot_protocol *bp=boot_protocol_get();struct display_identity identity;
    if(bp && bp->size>=BOOT_PROTOCOL_SIZE_V5 && bp->display_edid_size==128 &&
       !display_edid_parse(bp->display_edid,128,&identity))display_identity_describe(&identity,info);
    if(gpu && gpu->describe)gpu->describe(info);
    if(multi() && have_layout) {
        /* The one-monitor view of several: the primary output, its own
         * identity and the modes it can show. */
        int p=primary_index(&shown);
        const struct gpu_output *o=&info_of[p];
        info->width=shown.out[p].w;info->height=shown.out[p].h;
        if(o->model[0]) {
            memset(info->name,0,sizeof info->name);strncpy(info->name,o->model,sizeof info->name-1);
            memcpy(info->manufacturer,o->manufacturer,sizeof info->manufacturer);
            info->width_mm=o->width_mm;info->height_mm=o->height_mm;
        }
        if(o->mode_count) {
            info->mode_count=0;
            for(unsigned k=0;k<o->mode_count && info->mode_count<EMBK_MONITOR_MAX_MODES;k++)
                info->modes[info->mode_count++]=(struct embk_monitor_mode){o->modes[k].width,o->modes[k].height,o->modes[k].refresh_millihz,
                                                                           o->modes[k].preferred?EMBK_MONITOR_PREFERRED:0};
        }
    }
    if(state.previous && state.phase==EMBK_MONITOR_ERROR)info->flags&=~EMBK_MONITOR_MODESET;
    if(!fb_can_change_mode())info->flags&=~EMBK_MONITOR_MODESET;
    if(!(info->flags&EMBK_MONITOR_MODESET))info->mode_count=0;
    int found=0;for(unsigned i=0;i<info->mode_count;i++)if(info->modes[i].width==info->width && info->modes[i].height==info->height){info->modes[i].flags|=EMBK_MONITOR_CURRENT;found=1;}
    if(!found && info->mode_count<EMBK_MONITOR_MAX_MODES)info->modes[info->mode_count++]=(struct embk_monitor_mode){info->width,info->height,0,EMBK_MONITOR_CURRENT};
    info->phase=state.phase;info->generation=state.generation;info->token=state.token;info->last_error=state.error;
    if(state.owner && current_thread && state.owner==current_process->pid)info->flags|=EMBK_MONITOR_OWN_TRIAL;
    uint64_t now=time_get_ns()/1000000;
    if(state.phase==EMBK_MONITOR_TRIAL && state.deadline>now)info->remaining_ms=(uint32_t)(state.deadline-now);
}

static void describe_layout(struct embk_display_layout *d) {
    memset(d,0,sizeof *d);
    d->version=EMBK_DISPLAY_LAYOUT_VERSION;
    const fb_info_t *fb=fb_get_info();
    d->desktop_width=fb?fb->width:0;d->desktop_height=fb?fb->height:0;
    d->phase=state.phase;d->generation=state.generation;d->token=state.token;d->last_error=state.error;
    uint64_t now=time_get_ns()/1000000;
    if(state.phase==EMBK_MONITOR_TRIAL && state.deadline>now)d->remaining_ms=(uint32_t)(state.deadline-now);
    if(!multi() || !have_layout) {
        /* One output: the whole desktop. */
        d->count=1;
        struct embk_output *o=&d->outputs[0];
        o->flags=EMBK_OUTPUT_CONNECTED|EMBK_OUTPUT_ENABLED|EMBK_OUTPUT_PRIMARY;
        o->width=d->desktop_width;o->height=d->desktop_height;
        strcpy(o->name,"Display-1");
        return;
    }
    d->flags=EMBK_LAYOUT_MULTI|(shown.mirror?EMBK_LAYOUT_MIRROR:0);
    d->count=outputs_n;
    d->max_enabled=max_lit();
    for(unsigned i=0;i<outputs_n && i<EMBK_DISPLAY_MAX_OUTPUTS;i++) {
        struct embk_output *o=&d->outputs[i];
        const struct gpu_output *g=&info_of[i];
        const struct layout_out *l=&shown.out[i];
        o->flags=(g->connected?EMBK_OUTPUT_CONNECTED:0)|(l->enabled?EMBK_OUTPUT_ENABLED:0)|
                 (l->enabled && l->primary?EMBK_OUTPUT_PRIMARY:0)|(g->has_edid?EMBK_OUTPUT_EDID:0)|
                 (g->builtin?EMBK_OUTPUT_BUILTIN:0);
        o->x=l->x;o->y=l->y;o->width=l->enabled?l->w:0;o->height=l->enabled?l->h:0;
        o->width_mm=g->width_mm;o->height_mm=g->height_mm;
        memcpy(o->name,g->name,sizeof o->name);memcpy(o->model,g->model,sizeof o->model);
        memcpy(o->manufacturer,g->manufacturer,sizeof o->manufacturer);
        for(unsigned k=0;k<g->mode_count && k<16;k++) {
            struct embk_monitor_mode *m=&o->modes[o->mode_count++];
            m->width=g->modes[k].width;m->height=g->modes[k].height;m->refresh_millihz=g->modes[k].refresh_millihz;
            m->flags=(g->modes[k].preferred?EMBK_MONITOR_PREFERRED:0)|
                     (l->enabled && g->modes[k].width==l->w && g->modes[k].height==l->h?EMBK_MONITOR_CURRENT:0);
            if(m->flags&EMBK_MONITOR_CURRENT)o->refresh_millihz=m->refresh_millihz;
        }
    }
}

/* A request from Settings, checked against what each output can show. */
static int layout_from_request(const struct embk_layout_request *r,struct layout *l) {
    if(r->version!=EMBK_DISPLAY_LAYOUT_VERSION || r->count!=outputs_n)return -EMBK_EINVAL;
    memset(l,0,sizeof *l);
    l->count=outputs_n;l->mirror=(r->flags&EMBK_LAYOUT_MIRROR)!=0;
    for(unsigned i=0;i<outputs_n;i++) {
        struct layout_out *o=&l->out[i];
        o->connected=info_of[i].connected;
        o->enabled=r->out[i].enabled!=0;o->primary=r->out[i].primary!=0;
        o->x=r->out[i].x;o->y=r->out[i].y;o->w=r->out[i].width;o->h=r->out[i].height;
        if(o->enabled && !has_mode(i,o->w,o->h))return -EMBK_EINVAL;
    }
    if(lit(l)>max_lit())return -EMBK_EINVAL;
    layout_normalize(l);
    return layout_validate(l,0,0)==LAYOUT_OK?0:-EMBK_EINVAL;
}

static int allowed(void) {
    if(!current_thread || boot_recovery_mode() || session_input_owner() || !compositor_desktop_healthy())return 0;
    uint64_t caps=process_current_caps();
    if((caps&(EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER)))!=(EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER)))return 0;
    return (!current_process->session_id && (caps&EMBK_CAP_BIT(EMBK_CAP_SESSION))) || session_ui_allowed(current_process->pid);
}

static int queue_trial(void) {
    state.owner=current_process->pid;state.epoch=monitor_session_epoch();
    state.token=next_token++;state.phase=EMBK_MONITOR_QUEUED;state.error=0;
    return 0;
}

int monitor_control(unsigned op,void *data,unsigned bytes) {
    mutex_lock(&lock);int rc=0;
    if(op==EMBK_MONITOR_GET) {
        if(bytes!=sizeof(struct embk_monitor_info))rc=-EMBK_EINVAL;else describe(data);goto out;
    }
    if(op==EMBK_MONITOR_LAYOUT_GET) {
        if(bytes!=sizeof(struct embk_display_layout))rc=-EMBK_EINVAL;else describe_layout(data);goto out;
    }
    if(!allowed()){rc=-EMBK_EPERM;goto out;}
    if(op==EMBK_MONITOR_LAYOUT_APPLY) {
        if(bytes!=sizeof(struct embk_layout_request)){rc=-EMBK_EINVAL;goto out;}
        const struct embk_layout_request *r=data;
        if(!multi() || !have_layout){rc=-EMBK_ENOTSUP;goto out;}
        if(state.previous || (state.phase!=EMBK_MONITOR_IDLE && state.phase!=EMBK_MONITOR_ERROR)){rc=-EMBK_EBUSY;goto out;}
        if(r->generation!=state.generation){rc=-EMBK_EAGAIN;goto out;}
        struct layout want;
        if((rc=layout_from_request(r,&want)))goto out;
        if(same_layout(&want,&shown)){rc=-EMBK_EINVAL;goto out;}
        requested=want;layout_queued=1;
        rc=queue_trial();goto out;
    }
    if(bytes!=sizeof(struct embk_monitor_request)){rc=-EMBK_EINVAL;goto out;}
    const struct embk_monitor_request *r=data;
    if(r->version!=EMBK_MONITOR_VERSION){rc=-EMBK_EINVAL;goto out;}
    if(op==EMBK_MONITOR_APPLY) {
        if(state.previous || (state.phase!=EMBK_MONITOR_IDLE && state.phase!=EMBK_MONITOR_ERROR)){rc=-EMBK_EBUSY;goto out;}
        struct embk_monitor_info info;describe(&info);
        if(!(info.flags&EMBK_MONITOR_MODESET)){rc=-EMBK_ENOTSUP;goto out;}
        if(r->generation!=state.generation){rc=-EMBK_EAGAIN;goto out;}
        int found=0;for(unsigned i=0;i<info.mode_count;i++)if(r->width==info.modes[i].width && r->height==info.modes[i].height)found=1;
        if(!found || (r->width==info.width && r->height==info.height) || r->token || !next_token){rc=-EMBK_EINVAL;goto out;}
        state.width=r->width;state.height=r->height;
        if(multi() && have_layout){requested=resized_primary(r->width,r->height);layout_queued=1;}
        else layout_queued=0;
        rc=queue_trial();goto out;
    }
    if(state.owner!=current_process->pid || r->token!=state.token || !r->token){rc=-EMBK_EPERM;goto out;}
    if(op==EMBK_MONITOR_KEEP) {
        if(state.phase!=EMBK_MONITOR_TRIAL){rc=-EMBK_EBUSY;goto out;}
        if(time_get_ns()/1000000>=state.deadline || (state.epoch!=monitor_session_epoch() || session_input_owner())) {state.phase=EMBK_MONITOR_REVERTING;rc=-EMBK_ECANCELED;goto out;}
        struct fb_surface *old=state.previous;state.previous=NULL;state.phase=EMBK_MONITOR_IDLE;state.owner=0;
        layout_trial=0;
        fb_release_surface(old);kprintf("display: %s confirmed\n",multi() && have_layout?"arrangement":"mode");
    }else if(op==EMBK_MONITOR_REVERT) {
        if(state.phase==EMBK_MONITOR_QUEUED){state.phase=EMBK_MONITOR_IDLE;state.owner=0;layout_queued=0;}
        else if(state.phase==EMBK_MONITOR_TRIAL)state.phase=EMBK_MONITOR_REVERTING;
        else rc=-EMBK_EBUSY;
    }else rc=-EMBK_EINVAL;
out:mutex_unlock(&lock);return rc;
}

void monitor_tick(void) {
    if(!mutex_trylock(&lock))return;
    uint64_t now=time_get_ns()/1000000;
    /* Hotplug, only while nothing is being tried: a trial owns the screen. */
    static uint64_t next_poll;
    if(multi() && state.phase==EMBK_MONITOR_IDLE && !state.previous && now>=next_poll) {
        next_poll=now+1000;
        if(gpu_active()->poll_outputs())hotplug();
    }
    int abandoned=state.owner && (!process_alive(state.owner) || state.epoch!=monitor_session_epoch() || session_input_owner());
    if(state.phase==EMBK_MONITOR_QUEUED) {
        if(abandoned){state.phase=EMBK_MONITOR_ERROR;state.error=-EMBK_ECANCELED;state.owner=0;layout_queued=0;goto out;}
        state.phase=EMBK_MONITOR_APPLYING;unsigned w=state.width,h=state.height;
        int use_layout=layout_queued;layout_queued=0;
        struct layout want=requested;
        mutex_unlock(&lock);
        struct fb_surface *next=0,*previous=NULL;
        if(use_layout) {
            if(show_layout(&want,&previous))previous=NULL;
        }else {
            next=fb_prepare_mode(w,h);
            if(next)previous=compositor_select_display(next);
        }
        mutex_lock(&lock);
        if(!previous) {
            if(next)fb_release_surface(next);
            state.error=-EMBK_EIO;state.phase=EMBK_MONITOR_ERROR;state.owner=0;
        }else {
            if(use_layout){before_trial=shown;shown=want;layout_trial=1;}
            state.previous=previous;state.generation++;state.phase=EMBK_MONITOR_TRIAL;state.deadline=time_get_ns()/1000000+30000;
            kprintf("display: %s trial; confirmation required\n",use_layout?"arrangement":"mode");
        }
    }else if(state.phase==EMBK_MONITOR_REVERTING || (state.phase==EMBK_MONITOR_TRIAL && (abandoned || now>=state.deadline))) {
        state.phase=EMBK_MONITOR_REVERTING;
        struct fb_surface *discard;
        if(layout_trial) {
            struct gpu_layout gl;gpu_layout_of(&before_trial,&gl);
            struct layout_rect rects[LAYOUT_MAX_OUTPUTS];
            unsigned n=layout_rects(&before_trial,rects,LAYOUT_MAX_OUTPUTS);
            discard=compositor_select_layout(state.previous,&gl,rects,n);
        }else discard=compositor_select_display(state.previous);
        if(discard) {
            if(layout_trial){shown=before_trial;layout_trial=0;}
            fb_release_surface(discard);state.previous=NULL;state.phase=EMBK_MONITOR_IDLE;state.generation++;state.owner=0;state.error=0;
            kprintf("display: previous %s restored\n",multi() && have_layout?"arrangement":"mode");
        }else {state.phase=EMBK_MONITOR_ERROR;state.error=-EMBK_EIO;/* Retain both device-owned buffers. */}
    }
out:mutex_unlock(&lock);
}
