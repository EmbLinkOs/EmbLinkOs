#include "ipc/notify.h"
#include "process/session.h"
#include "process/process.h"
#include "arch/x86_64/boot/boot_protocol.h"
#include "gfx/compositor.h"
#include "ipc/clipboard.h"
#include "drivers/input/keyboard.h"
#include "include/errno.h"
#include "include/kprintf.h"

static uint32_t controller,epoch,gate=UINT32_MAX;
static unsigned flags=EMBK_SESSION_LOCKED;
static unsigned repaint;
uint32_t session_active_epoch(void) {
    unsigned f=__atomic_load_n(&flags,__ATOMIC_ACQUIRE);
    return (f&EMBK_SESSION_ACTIVE) && !(f&EMBK_SESSION_ENDING)?__atomic_load_n(&epoch,__ATOMIC_ACQUIRE):0;
}
int session_managed(void) {return __atomic_load_n(&controller,__ATOMIC_ACQUIRE)!=0;}
int session_ui_allowed(int pid) {
    uint32_t owner=__atomic_load_n(&gate,__ATOMIC_ACQUIRE);
    if(owner)return owner==(uint32_t)pid;
    if(pid>0 && controller) {
        struct process *p=process_find((uint32_t)pid);
        if(p && p->session_id && p->session_id!=epoch)return 0;
    }
    return 1;
}
uint32_t session_input_owner(void) {return __atomic_load_n(&gate,__ATOMIC_ACQUIRE);}
void session_bind_init(uint32_t pid) {
    const struct boot_protocol *bp=boot_protocol_get();
    if(bp && bp->boot_mode==3) flags|=EMBK_SESSION_LIVE;
    __atomic_store_n(&controller,pid,__ATOMIC_RELEASE);
}
static void changed(void) {
    __atomic_store_n(&repaint,1,__ATOMIC_RELEASE);
}
void session_refresh_screen(void) {
    if(__atomic_exchange_n(&repaint,0,__ATOMIC_ACQ_REL)) {
        keyboard_flush_input();
        compositor_session_changed();
    }
}
static void lock_now(void) {
    if(!(flags&EMBK_SESSION_ACTIVE) || (flags&(EMBK_SESSION_LIVE|EMBK_SESSION_ENDING))) return;
    if(flags&EMBK_SESSION_LOCKED) return;
    flags|=EMBK_SESSION_LOCKED;
    __atomic_store_n(&gate,UINT32_MAX,__ATOMIC_RELEASE);
    clipboard_clear();changed();
}
void session_request_lock(void) {
    sched_lock();lock_now();sched_unlock();
}
int session_may_spawn(const struct process *p) {
    return !p || p->pid==controller || !p->session_id ||
        (p->session_id==epoch && !(flags&EMBK_SESSION_ENDING));
}
int session_may_signal(const struct process *p,uint32_t target) {
    if(!p) return 0;
    if(p->pid==controller) return 1;
    if(target==controller || (__atomic_load_n(&gate,__ATOMIC_ACQUIRE) && target==gate)) return 0;
    struct process *t=process_find(target);
    if(!t) return 0;
    return p->session_id && p->session_id==t->session_id;
}
int session_control(struct process *p,unsigned command,int arg) {
    if(!p || !controller) return -EMBK_ENODEV;
    if(command==EMBK_SESSION_STATUS) return __atomic_load_n(&flags,__ATOMIC_ACQUIRE);
    if(command==EMBK_SESSION_LOCK || command==EMBK_SESSION_LOGOUT) {
        sched_lock();
        if(!p->session_id || p->session_id!=epoch || !(flags&EMBK_SESSION_ACTIVE)) {
            sched_unlock();return -EMBK_EPERM;
        }
        if(command==EMBK_SESSION_LOCK) {
            if(flags&EMBK_SESSION_LIVE) {sched_unlock();return -EMBK_ENOTSUP;}
            lock_now();
        } else {
            flags|=EMBK_SESSION_ENDING|EMBK_SESSION_LOCKED;
            __atomic_store_n(&gate,UINT32_MAX,__ATOMIC_RELEASE);clipboard_clear();changed();
        }
        sched_unlock();return 0;
    }
    if(p->pid!=controller) return -EMBK_EPERM;
    if(command==EMBK_SESSION_END) {
        /* Freeze further spawns first, then kill every member of the epoch,
         * including detached grandchildren. PID slots never define membership. */
        sched_lock();flags|=EMBK_SESSION_ENDING|EMBK_SESSION_LOCKED;
        __atomic_store_n(&gate,UINT32_MAX,__ATOMIC_RELEASE);p->session_id=0;sched_unlock();
        int live=process_end_session(epoch,controller);
        clipboard_clear();changed();return live?-EMBK_EBUSY:0;
    }
    if(command==EMBK_SESSION_GREETER) {
        uint32_t pid;
        if(!(flags&EMBK_SESSION_LOCKED) || process_handle_resolve(p,arg,&pid)) return -EMBK_EINVAL;
        __atomic_store_n(&gate,pid,__ATOMIC_RELEASE);changed();return 0;
    }
    if(command==EMBK_SESSION_BEGIN) {
        if((flags&EMBK_SESSION_ACTIVE) && !(flags&EMBK_SESSION_ENDING)) return -EMBK_EBUSY;
        notify_clear();
        epoch++;if(!epoch) epoch++;
        p->session_id=epoch;
        flags=(flags&EMBK_SESSION_LIVE)|EMBK_SESSION_ACTIVE|EMBK_SESSION_LOCKED;
        __atomic_store_n(&gate,UINT32_MAX,__ATOMIC_RELEASE);clipboard_clear();changed();return 0;
    }
    if(command==EMBK_SESSION_UNLOCK) {
        if(flags&EMBK_SESSION_ENDING) return -EMBK_EBUSY;
        flags&=~EMBK_SESSION_LOCKED;
        __atomic_store_n(&gate,0,__ATOMIC_RELEASE);changed();return 0;
    }
    return -EMBK_EINVAL;
}
