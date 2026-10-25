#include "control.h"
#include "edid.h"
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
static int allowed(void) {
    if(!current_thread || boot_recovery_mode() || session_input_owner() || !compositor_desktop_healthy())return 0;
    uint64_t caps=process_current_caps();
    if((caps&(EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER)))!=(EMBK_CAP_BIT(EMBK_CAP_GPU)|EMBK_CAP_BIT(EMBK_CAP_POWER)))return 0;
    return (!current_process->session_id && (caps&EMBK_CAP_BIT(EMBK_CAP_SESSION))) || session_ui_allowed(current_process->pid);
}
int monitor_control(unsigned op,void *data,unsigned bytes) {
    mutex_lock(&lock);int rc=0;
    if(op==EMBK_MONITOR_GET) {
        if(bytes!=sizeof(struct embk_monitor_info))rc=-EMBK_EINVAL;else describe(data);goto out;
    }
    if(!allowed()){rc=-EMBK_EPERM;goto out;}
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
        state.width=r->width;state.height=r->height;state.owner=current_process->pid;state.epoch=monitor_session_epoch();
        state.token=next_token++;state.phase=EMBK_MONITOR_QUEUED;state.error=0;goto out;
    }
    if(state.owner!=current_process->pid || r->token!=state.token || !r->token){rc=-EMBK_EPERM;goto out;}
    if(op==EMBK_MONITOR_KEEP) {
        if(state.phase!=EMBK_MONITOR_TRIAL){rc=-EMBK_EBUSY;goto out;}
        if(time_get_ns()/1000000>=state.deadline || (state.epoch!=monitor_session_epoch() || session_input_owner())) {state.phase=EMBK_MONITOR_REVERTING;rc=-EMBK_ECANCELED;goto out;}
        struct fb_surface *old=state.previous;state.previous=NULL;state.phase=EMBK_MONITOR_IDLE;state.owner=0;
        fb_release_surface(old);kprintf("display: mode %ux%u confirmed\n",state.width,state.height);
    }else if(op==EMBK_MONITOR_REVERT) {
        if(state.phase==EMBK_MONITOR_QUEUED){state.phase=EMBK_MONITOR_IDLE;state.owner=0;}
        else if(state.phase==EMBK_MONITOR_TRIAL)state.phase=EMBK_MONITOR_REVERTING;
        else rc=-EMBK_EBUSY;
    }else rc=-EMBK_EINVAL;
out:mutex_unlock(&lock);return rc;
}
void monitor_tick(void) {
    if(!mutex_trylock(&lock))return;
    uint64_t now=time_get_ns()/1000000;
    int abandoned=state.owner && (!process_alive(state.owner) || state.epoch!=monitor_session_epoch() || session_input_owner());
    if(state.phase==EMBK_MONITOR_QUEUED) {
        if(abandoned){state.phase=EMBK_MONITOR_ERROR;state.error=-EMBK_ECANCELED;state.owner=0;goto out;}
        state.phase=EMBK_MONITOR_APPLYING;unsigned w=state.width,h=state.height;
        mutex_unlock(&lock);
        struct fb_surface *next=fb_prepare_mode(w,h),*previous=NULL;
        if(next)previous=compositor_select_display(next);
        mutex_lock(&lock);
        if(!previous) {
            if(next)fb_release_surface(next);
            state.error=-EMBK_EIO;state.phase=EMBK_MONITOR_ERROR;state.owner=0;
        }else {
            state.previous=previous;state.generation++;state.phase=EMBK_MONITOR_TRIAL;state.deadline=time_get_ns()/1000000+30000;
            kprintf("display: mode %ux%u trial; confirmation required\n",w,h);
        }
    }else if(state.phase==EMBK_MONITOR_REVERTING || (state.phase==EMBK_MONITOR_TRIAL && (abandoned || now>=state.deadline))) {
        state.phase=EMBK_MONITOR_REVERTING;
        struct fb_surface *discard=compositor_select_display(state.previous);
        if(discard) {
            fb_release_surface(discard);state.previous=NULL;state.phase=EMBK_MONITOR_IDLE;state.generation++;state.owner=0;state.error=0;
            kprintf("display: previous mode restored\n");
        }else {state.phase=EMBK_MONITOR_ERROR;state.error=-EMBK_EIO;/* Retain both device-owned buffers. */}
    }
out:mutex_unlock(&lock);
}
