/* Screen ownership layered on the upstream session IDs and safe teardown.
 * Init still authenticates and opens sessions; the trusted session leader
 * owns its password surface. Background apps keep running while locked. */
#include "process/session.h"
#include "process/process.h"
#include "boot/boot_protocol.h"
#include "gfx/compositor.h"
#include "drivers/input/keyboard.h"
#include "ipc/clipboard.h"
#include "ipc/notify.h"
#include "include/errno.h"
#include "include/spinlock.h"
static spinlock_t state_lock=SPINLOCK_INIT;
static uint32_t leader,epoch,gate;
static unsigned flags,repaint;
uint32_t session_active_epoch(void) {return __atomic_load_n(&epoch,__ATOMIC_ACQUIRE);}
int session_managed(void) {return session_active_epoch()!=0;}
uint32_t session_input_owner(void) {return __atomic_load_n(&gate,__ATOMIC_ACQUIRE);}
int session_ui_allowed(int pid) {uint32_t owner=session_input_owner();return !owner || owner==(uint32_t)pid;}
static void changed(void) {__atomic_store_n(&repaint,1,__ATOMIC_RELEASE);}
void session_request_lock(void) {
    spin_lock(&state_lock);
    if(!(flags&EMBK_SESSION_ACTIVE) || (flags&(EMBK_SESSION_LIVE|EMBK_SESSION_LOCKED|EMBK_SESSION_ENDING))) {
        spin_unlock(&state_lock);return;
    }
    __atomic_fetch_or(&flags,EMBK_SESSION_LOCKED,__ATOMIC_ACQ_REL);
    __atomic_store_n(&gate,leader,__ATOMIC_RELEASE);
    uint32_t sid=epoch;changed();spin_unlock(&state_lock);
    clipboard_session_ended(sid);
}
void session_refresh_screen(void) {
    uint32_t owner=__atomic_load_n(&leader,__ATOMIC_ACQUIRE),sid=0,expected=session_active_epoch();
    if(owner && (process_session_of(owner,&sid) || sid!=expected)) {
        spin_lock(&state_lock);
        if(leader==owner && epoch==expected) {
        __atomic_store_n(&gate,0,__ATOMIC_RELEASE);
        __atomic_store_n(&leader,0,__ATOMIC_RELEASE);
        __atomic_store_n(&epoch,0,__ATOMIC_RELEASE);
        __atomic_store_n(&flags,0,__ATOMIC_RELEASE);notify_clear();changed();
        }
        spin_unlock(&state_lock);
    }
    if(__atomic_exchange_n(&repaint,0,__ATOMIC_ACQ_REL)) {
        keyboard_flush_input();compositor_session_changed();
    }
}
int session_control(struct process *p,unsigned command,int arg) {
    (void)arg;if(!p)return -EMBK_EPERM;
    if(command==EMBK_SESSION_BEGIN) {
        if(!p->session_leader || !p->session_id)return -EMBK_EPERM;
        unsigned live=p->session_live?EMBK_SESSION_LIVE:0;
        spin_lock(&state_lock);notify_clear();
        __atomic_store_n(&leader,p->pid,__ATOMIC_RELEASE);
        __atomic_store_n(&epoch,p->session_id,__ATOMIC_RELEASE);
        __atomic_store_n(&flags,EMBK_SESSION_ACTIVE|live,__ATOMIC_RELEASE);
        __atomic_store_n(&gate,0,__ATOMIC_RELEASE);changed();spin_unlock(&state_lock);return 0;
    }
    if(!p->session_id || p->session_id!=session_active_epoch())return -EMBK_EPERM;
    if(command==EMBK_SESSION_STATUS)return __atomic_load_n(&flags,__ATOMIC_ACQUIRE);
    if(command==EMBK_SESSION_LOCK) {
        if(flags&EMBK_SESSION_LIVE)return -EMBK_ENOTSUP;
        session_request_lock();return 0;
    }
    spin_lock(&state_lock);
    if(p->pid!=leader || p->session_id!=epoch){spin_unlock(&state_lock);return -EMBK_EPERM;}
    if(command==EMBK_SESSION_UNLOCK) {
        if(flags&EMBK_SESSION_ENDING){spin_unlock(&state_lock);return -EMBK_EBUSY;}
        __atomic_fetch_and(&flags,~EMBK_SESSION_LOCKED,__ATOMIC_ACQ_REL);
        __atomic_store_n(&gate,0,__ATOMIC_RELEASE);changed();spin_unlock(&state_lock);return 0;
    }
    if(command==EMBK_SESSION_LOGOUT || command==EMBK_SESSION_END) {
        __atomic_fetch_or(&flags,EMBK_SESSION_ENDING|EMBK_SESSION_LOCKED,__ATOMIC_ACQ_REL);
        __atomic_store_n(&gate,p->pid,__ATOMIC_RELEASE);changed();spin_unlock(&state_lock);
        clipboard_session_ended(p->session_id);return 0;
    }
    spin_unlock(&state_lock);return -EMBK_EINVAL;
}
