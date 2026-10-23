#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "ipc/notify.c"
static uint64_t now;
static int unlocked=1;
void spin_lock(spinlock_t *p){assert(!p->locked);p->locked=1;}
void spin_unlock(spinlock_t *p){assert(p->locked);p->locked=0;}
uint64_t timer_uptime_ms(void){return now;}
uint32_t session_active_epoch(void){return 7;}
int session_ui_allowed(int pid){(void)pid;return unlocked;}
int main(void){
    struct process p={.pid=5,.session_id=7},outsider={.pid=6,.session_id=8};
    struct embk_notice_request r={.version=1,.severity=0,.title="Files",.body="Copy complete"};
    struct embk_notice_info info;
    assert(notify_post(&outsider,&r)==-EMBK_EPERM);assert(!notify_post(&p,&r));assert(notify_post(&p,&r)==-EMBK_EAGAIN);
    for(unsigned i=0;i<12;i++){now+=1100;assert(!notify_post(&p,&r));}
    assert(!notify_snapshot(&p,&info,0) && info.count==8 && info.entries[0].sequence>info.entries[7].sequence);
    memset(r.title,'x',sizeof r.title);now+=1100;assert(notify_post(&p,&r)==-EMBK_EINVAL);
    unlocked=0;assert(notify_snapshot(&p,&info,0)==-EMBK_EPERM);unlocked=1;
    assert(!notify_snapshot(&p,&info,1) && !info.count);
    notify_system(1,"Battery","Low charge");assert(!notify_snapshot(&p,&info,0)&&info.entries[0].source_pid==0);
    puts("notifications: epoch isolation, bounded history, source IDs, rate limits, string bounds and lock privacy OK");
}
