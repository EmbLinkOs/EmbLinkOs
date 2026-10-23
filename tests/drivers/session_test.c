#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "process/session.c"
static struct process init,app,greeter,outsider;
static struct boot_protocol bp;
static unsigned clears,paints,flushed,depth;
void sched_lock(void){assert(!depth);depth=1;}
void sched_unlock(void){assert(depth);depth=0;}
void clipboard_clear(void){clears++;}
void notify_clear(void){}
void keyboard_flush_input(void){flushed++;}
void compositor_session_changed(void){paints++;}
const struct boot_protocol *boot_protocol_get(void){return &bp;}
struct process *process_find(uint32_t pid){if(pid==init.pid)return &init;if(pid==app.pid)return &app;if(pid==greeter.pid)return &greeter;return NULL;}
int process_handle_resolve(struct process *p,int h,uint32_t *pid){if(p!=&init || h!=99)return -1;*pid=greeter.pid;return 0;}
int process_end_session(uint32_t e,uint32_t root){assert(e==app.session_id && root==init.pid);app.live_thread_count=0;return 0;}
int main(void){
    init.pid=1;app.pid=2;greeter.pid=3;outsider.pid=4;session_bind_init(1);
    assert(session_control(&app,EMBK_SESSION_BEGIN,0)==-EMBK_EPERM);
    assert(!session_ui_allowed(2));assert(!session_control(&init,EMBK_SESSION_GREETER,99));
    assert(session_ui_allowed(3) && !session_ui_allowed(2));
    assert(!session_control(&init,EMBK_SESSION_BEGIN,0));app.session_id=init.session_id;
    assert(!session_control(&init,EMBK_SESSION_UNLOCK,0) && session_ui_allowed(2));
    assert(session_control(&outsider,EMBK_SESSION_LOCK,0)==-EMBK_EPERM);
    assert(!session_control(&app,EMBK_SESSION_LOCK,0));assert(!session_ui_allowed(2));
    assert(session_control(&app,EMBK_SESSION_UNLOCK,0)==-EMBK_EPERM);
    assert(!session_control(&init,EMBK_SESSION_GREETER,99));
    assert(session_input_owner()==3 && !session_may_signal(&app,1) && !session_may_signal(&app,3));
    assert(!session_control(&init,EMBK_SESSION_UNLOCK,0));
    assert(!session_control(&app,EMBK_SESSION_LOGOUT,0));assert(!session_may_spawn(&app));
    assert(!session_control(&init,EMBK_SESSION_END,0) && !init.session_id);
    uint32_t previous=epoch;assert(!session_control(&init,EMBK_SESSION_BEGIN,0) && epoch!=previous);
    assert(session_control(&app,EMBK_SESSION_LOGOUT,0)==-EMBK_EPERM);
    session_refresh_screen();assert(paints && flushed && clears);
    puts("session: init-only authentication, compositor/input gate, protected greeter, logout epoch and spawn refusal OK");
}
