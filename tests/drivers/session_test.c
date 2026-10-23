#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "process/session.c"
static struct process desktop,app,outsider;
static struct boot_protocol bp;
static unsigned clears,paints,flushed,alive=1;
void spin_lock(spinlock_t *p){assert(!p->locked);p->locked=1;}
void spin_unlock(spinlock_t *p){assert(p->locked);p->locked=0;}
void clipboard_session_ended(uint32_t sid){assert(sid==7);clears++;}
void notify_clear(void){}
void keyboard_flush_input(void){flushed++;}
void compositor_session_changed(void){paints++;}
const struct boot_protocol *boot_protocol_get(void){return &bp;}
int process_session_of(uint32_t pid,uint32_t *sid){if(pid!=desktop.pid || !alive)return -EMBK_ENOENT;*sid=desktop.session_id;return 0;}
int main(void){
    desktop.pid=1;desktop.session_id=7;desktop.session_leader=true;
    app.pid=2;app.session_id=7;outsider.pid=3;outsider.session_id=8;
    assert(session_control(&app,EMBK_SESSION_BEGIN,0)==-EMBK_EPERM);
    assert(!session_control(&desktop,EMBK_SESSION_BEGIN,0));
    assert(session_active_epoch()==7 && session_ui_allowed(app.pid));
    assert(session_control(&outsider,EMBK_SESSION_LOCK,0)==-EMBK_EPERM);
    assert(!session_control(&app,EMBK_SESSION_LOCK,0));
    assert(session_input_owner()==desktop.pid && !session_ui_allowed(app.pid) && session_ui_allowed(desktop.pid));
    assert(session_control(&app,EMBK_SESSION_UNLOCK,0)==-EMBK_EPERM);
    assert(!session_control(&desktop,EMBK_SESSION_UNLOCK,0) && session_ui_allowed(app.pid));
    assert(!session_control(&desktop,EMBK_SESSION_LOGOUT,0));
    assert(session_control(&desktop,EMBK_SESSION_UNLOCK,0)==-EMBK_EBUSY);
    alive=0;session_refresh_screen();assert(!session_active_epoch() && session_ui_allowed(outsider.pid));
    assert(paints && flushed && clears);
    alive=1;bp.boot_mode=BOOT_MODE_LIVE;desktop.session_live=true;
    assert(!session_control(&desktop,EMBK_SESSION_BEGIN,0));
    assert(session_control(&app,EMBK_SESSION_LOCK,0)==-EMBK_ENOTSUP);
    session_request_lock();assert(session_ui_allowed(app.pid));
    puts("session: trusted leader, session isolation, lock ownership, logout retirement and explicit live mode OK");
}
