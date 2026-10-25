/* Exercise the production pipe state machine; real waits/descriptor ownership
 * are covered by processprobe in the guest, not simulated by this shim. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/ipc/pipe.h"
#define __PROCESS_H__
#include "../../kernel/ipc/handle.h"
#include "include/errno.h"
struct wait_queue { unsigned dummy; };
struct process { bool cancelled;struct obj_handle obj_handles[OBJ_HANDLE_MAX]; };
static struct process owner;
static struct process *current_process=&owner;
static int locked;
static void sched_lock(void){assert(!locked);locked=1;}
static void sched_unlock(void){assert(locked);locked=0;}
static void wait_queue_wake_all(struct wait_queue *q){(void)q;assert(locked);}
static void sched_block_current_locked(struct wait_queue *q){(void)q;abort();}
int obj_handle_alloc(struct process *p,enum handle_kind k,void *obj) {
    for(int i=0;i<OBJ_HANDLE_MAX;i++)if(!p->obj_handles[i].used){p->obj_handles[i]=(struct obj_handle){.used=true,.kind=k,.obj=obj};return i;}
    return -EMBK_EMFILE;
}
void obj_handle_free(struct process *p,int h) {
    if(!p->obj_handles[h].used)return;
    sched_lock();
    if(p->obj_handles[h].kind==HANDLE_KIND_PIPE)pipe_release_for_handle_locked(p->obj_handles[h].obj);
    p->obj_handles[h]=(struct obj_handle){0};sched_unlock();
}
#include "../../kernel/ipc/pipe.c"
static unsigned seed=0x274533;
static unsigned random_next(void){seed=seed*1664525u+1013904223u;return seed;}
static struct pipe *make(int handles[2]) {
    assert(pipe_create(&handles[0],&handles[1])==0);
    return ((struct pipe_end *)owner.obj_handles[handles[0]].obj)->p;
}
static void stream(void) {
    int h[2];struct pipe *p=make(h);
    unsigned char model[PIPE_BUF_SIZE],bytes[PIPE_BUF_SIZE*2],got[333];size_t count=0,total=0;
    for(int step=0;step<20000;step++) {
        size_t n=0,len;
        if((random_next()>>16)&1) {
            len=random_next()%sizeof bytes;for(size_t i=0;i<len;i++)bytes[i]=(unsigned char)random_next();
            size_t available=sizeof model-count;
            int rc=pipe_write_flags(p,bytes,len,&n,true,true);
            if(len && (available==0 || (len<=PIPE_BUF_SIZE && len>available)))assert(rc==-EMBK_EAGAIN && n==0);
            else {assert(rc==0 && n<=len && n<=available);memcpy(model+count,bytes,n);count+=n;total+=n;}
        } else {
            len=random_next()%sizeof got;int rc=pipe_read_flags(p,got,len,&n,true);
            if(len && !count)assert(rc==-EMBK_EAGAIN && n==0);
            else {assert(rc==0 && n<=count && !memcmp(model,got,n));count-=n;memmove(model,model+n,count);}
        }
        assert(pipe_avail(p,0)==(int64_t)count);
    }
    assert(total>500000);obj_handle_free(&owner,h[1]);
    while(count){size_t n=0;assert(pipe_read_flags(p,got,sizeof got,&n,true)==0 && n && !memcmp(model,got,n));count-=n;memmove(model,model+n,count);}
    size_t n=123;owner.cancelled=true;
    assert(pipe_read_flags(p,got,1,&n,true)==0 && n==0); /* EOF beats cancellation */
    owner.cancelled=false;obj_handle_free(&owner,h[0]);assert(pipe_live_count()==0);
}
int main(void) {
    stream();
    int h[2];struct pipe *p=make(h);char b[PIPE_BUF_SIZE+1];memset(b,'x',sizeof b);size_t n=0;
    assert(pipe_write_flags(p,b,PIPE_BUF_SIZE-1,&n,true,true)==0 && n==PIPE_BUF_SIZE-1);
    n=0;assert(pipe_write_flags(p,b,2,&n,true,true)==-EMBK_EAGAIN && n==0);
    assert(pipe_write(p,b,2,&n)==0 && n==1); /* legacy partial writes preserved */
    n=0;assert(pipe_write_flags(p,b,sizeof b,&n,true,true)==-EMBK_EAGAIN && n==0);
    owner.cancelled=true;assert(pipe_write_flags(p,b,1,&n,false,true)==-EMBK_ECANCELED);
    obj_handle_free(&owner,h[0]);assert(pipe_write_flags(p,b,1,&n,true,true)==-EMBK_EPIPE);
    obj_handle_free(&owner,h[1]);owner.cancelled=false;assert(pipe_live_count()==0);
    int pairs[OBJ_HANDLE_MAX/2][2];
    for(unsigned i=0;i<sizeof pairs/sizeof pairs[0];i++)make(pairs[i]);
    assert(pipe_create(&h[0],&h[1])==-EMBK_EMFILE && pipe_live_count()==OBJ_HANDLE_MAX/2);
    for(unsigned i=0;i<sizeof pairs/sizeof pairs[0];i++){obj_handle_free(&owner,pairs[i][0]);obj_handle_free(&owner,pairs[i][1]);}
    for(int i=0;i<OBJ_HANDLE_MAX-1;i++)owner.obj_handles[i].used=true;
    assert(pipe_create(&h[0],&h[1])==-EMBK_EMFILE && pipe_live_count()==0 && !owner.obj_handles[OBJ_HANDLE_MAX-1].used);
    puts("PASS: pipe byte streams, atomic/nonblocking writes, EOF/cancellation ordering and handle-allocation rollback");
}
