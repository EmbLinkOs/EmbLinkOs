#include "ipc/notify.h"
#include "process/session.h"
#include "process/process.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "include/errno.h"
static spinlock_t lock=SPINLOCK_INIT;
static struct embk_notice ring[EMBK_NOTIFY_MAX];
static unsigned head,count,burst;
static uint64_t sequence,burst_start;
static int valid(const char *p,unsigned n,int newline) {
    for(unsigned i=0;i<n;) {
        unsigned char c=p[i];if(!c)return i!=0;
        if((c<32 && !(newline && c=='\n')) || c==127)return 0;
        if(c<128){i++;continue;}
        unsigned size=c>=0xc2 && c<=0xdf?2:c>=0xe0 && c<=0xef?3:c>=0xf0 && c<=0xf4?4:0;
        if(!size || size>n-i)return 0;
        uint32_t value=c&((1u<<(7-size))-1);
        for(unsigned j=1;j<size;j++) {
            unsigned char tail=p[i+j];if((tail&0xc0)!=0x80)return 0;
            value=(value<<6)|(tail&63);
        }
        if((size==2 && value<128) || (size==3 && value<2048) || (size==4 && value<65536) ||
           value>0x10ffff || (value>=0xd800 && value<=0xdfff))return 0;
        i+=size;
    }
    return 0;
}
void notify_clear(void) {
    spin_lock(&lock);memset(ring,0,sizeof ring);head=count=burst=0;burst_start=0;spin_unlock(&lock);
}
static int post(uint32_t pid,const struct embk_notice_request *r) {
    uint64_t now=timer_uptime_ms();spin_lock(&lock);
    if(now-burst_start>=1000){burst=0;burst_start=now;}
    if(burst>=8){spin_unlock(&lock);return -EMBK_EAGAIN;}
    for(unsigned i=0;pid && i<count;i++)
        if(ring[i].source_pid==pid && now-ring[i].created_ms<1000){spin_unlock(&lock);return -EMBK_EAGAIN;}
    struct embk_notice *n=&ring[head];memset(n,0,sizeof *n);
    n->sequence=++sequence;n->created_ms=now;n->source_pid=pid;n->severity=r->severity;
    snprintf(n->title,sizeof n->title,"%s",r->title);snprintf(n->body,sizeof n->body,"%s",r->body);
    head=(head+1)%EMBK_NOTIFY_MAX;if(count<EMBK_NOTIFY_MAX)count++;burst++;
    spin_unlock(&lock);return 0;
}
int notify_post(struct process *p,const struct embk_notice_request *r) {
    if(!p || !p->session_id || p->session_id!=session_active_epoch())return -EMBK_EPERM;
    if(r->version!=EMBK_NOTIFY_VERSION || r->severity>2 ||
       !valid(r->title,sizeof r->title,0) || (r->body[0] && !valid(r->body,sizeof r->body,1)))return -EMBK_EINVAL;
    return post(p->pid,r);
}
int notify_snapshot(struct process *p,struct embk_notice_info *out,int clear) {
    if(!p || !p->session_id || p->session_id!=session_active_epoch() || !session_ui_allowed(p->pid))return -EMBK_EPERM;
    if(clear)notify_clear();
    spin_lock(&lock);memset(out,0,sizeof *out);out->version=EMBK_NOTIFY_VERSION;out->count=count;out->latest=sequence;
    for(unsigned i=0;i<count;i++)out->entries[i]=ring[(head+EMBK_NOTIFY_MAX-1-i)%EMBK_NOTIFY_MAX];
    spin_unlock(&lock);return 0;
}
void notify_system(unsigned severity,const char *title,const char *body) {
    if(!session_active_epoch())return;
    struct embk_notice_request r={.version=EMBK_NOTIFY_VERSION,.severity=severity};
    snprintf(r.title,sizeof r.title,"%s",title);snprintf(r.body,sizeof r.body,"%s",body);post(0,&r);
}
