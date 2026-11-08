/* Exercise the production PTY state machine under host sanitizers. Blocking,
 * real descriptor inheritance and wakeups are covered by `test terminal`. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../../kernel/tty/pty.h"
#define __PROCESS_H__
#define __FD_H__
#define __KMALLOC_H__
static unsigned allocated;
static void *kcalloc(uint64_t n,uint64_t size) { void *p=calloc(n,size); allocated+=p!=NULL; return p; }
static void kfree(void *p) { if (p) allocated--; free(p); }
#define POLLIN 1
#define POLLOUT 4
#define POLLHUP 16
struct wait_queue { unsigned dummy; };
struct process { bool cancelled; };
static struct process process;
static struct process *current_process=&process;
static void *current_thread=&process;
static int locked;
static unsigned interrupted,cancelled;
static void sched_lock(void) { assert(!locked); locked=1; }
static void sched_unlock(void) { assert(locked); locked=0; }
static void wait_queue_wake_all(struct wait_queue *q) { (void)q; assert(locked); }
static void sched_block_current_locked(struct wait_queue *q) { (void)q; abort(); }
static int process_cancel_locked(uint32_t pid) { assert(locked && pid==42); cancelled++; return 0; }
static int process_cancel(uint32_t pid) { assert(!locked && pid==42); cancelled++; return 0; }
static int process_raise_interrupt(uint32_t pid) { assert(!locked && pid==42); interrupted++; return 1; }
#include "../../kernel/tty/pty.c"

static unsigned seed=0x179362;
static unsigned random_next(void) { seed=seed*1664525u+1013904223u; return seed; }
static void close_pair(struct pty *p) { sched_lock(); pty_unref_locked(p,0); pty_unref_locked(p,1); sched_unlock(); }
static void stream(struct pty *p,int side) {
    unsigned char model[32768],bytes[512],got[512]; size_t count=0,total_written=0,total_read=0;
    for (int step=0;step<10000;step++) {
        size_t len=random_next()%sizeof bytes,n=0;
        if ((random_next()>>16)&1) {
            for (size_t i=0;i<len;i++) bytes[i]=(unsigned char)random_next();
            int rc=pty_write(p,side,bytes,len,&n,true);
            assert(rc==0 || rc==-EMBK_EAGAIN);
            assert(n<=len && count+n<=sizeof model);
            memcpy(model+count,bytes,n); count+=n; total_written+=n;
        } else {
            int rc=pty_read(p,!side,got,len,&n,true);
            assert(rc==0 || rc==-EMBK_EAGAIN);
            assert(n<=len && n<=count && !memcmp(got,model,n));
            count-=n; total_read+=n; memmove(model,model+n,count);
        }
        assert(pty_avail(p,!side)==(int64_t)count);
    }
    assert(total_written>500000 && total_read>500000);
    while (count) {
        size_t n=0; assert(pty_read(p,!side,got,sizeof got,&n,true)==0 && n);
        assert(n<=count && !memcmp(got,model,n)); count-=n; memmove(model,model+n,count);
    }
}
int main(void) {
    struct pty *p; assert(pty_create(&p)==0);
    struct embk_terminal_attr raw={0}; raw.cc[EMBK_TCC_MIN]=1;
    assert(pty_control(p,1,EMBK_TERMINAL_SET_ATTR,&raw)==0);
    stream(p,0); stream(p,1);
    struct embk_terminal_attr canon=raw; canon.lflag=EMBK_TLF_ICANON; canon.cc[EMBK_TCC_EOF]=4;
    unsigned char bytes[1200],got[1200]; memset(bytes,'w',sizeof bytes);
    for (size_t len=0;len<sizeof bytes-1;len+=17) {
        size_t n=0,r=0;
        assert(pty_control(p,1,EMBK_TERMINAL_SET_ATTR_FLUSH,&canon)==0);
        assert(pty_write(p,0,bytes,len,&n,true)==0 && n==len);
        assert(pty_avail(p,1)==0);
        assert(pty_write(p,0,"\n",1,&n,true)==0 && n==1);
        assert(pty_read(p,1,got,sizeof got,&r,true)==0);
        size_t kept=len<1023?len:1023;
        assert(r==kept+1 && !memcmp(got,bytes,kept) && got[kept]=='\n');
    }
    size_t n;
    assert(pty_write(p,0,"\004x\n",3,&n,true)==0 && n==3);
    assert(pty_read(p,1,got,sizeof got,&n,true)==0 && n==0);
    assert(pty_read(p,1,got,sizeof got,&n,true)==0 && n==2 && got[0]=='x');
    canon.lflag|=EMBK_TLF_ISIG; canon.cc[EMBK_TCC_INTR]=3;
    assert(pty_control(p,1,EMBK_TERMINAL_SET_ATTR,&canon)==0);
    uint32_t fg=42; assert(pty_control(p,1,EMBK_TERMINAL_FOREGROUND,&fg)==0);
    assert(pty_write(p,0,"\003",1,&n,true)==0 && interrupted==1);
    sched_lock(); pty_pin_locked(p); pty_unref_locked(p,0); pty_unref_locked(p,1); sched_unlock();
    assert(pty_live_count()==1 && cancelled==1);
    assert(pty_read(p,1,got,sizeof got,&n,true)==0 && n==0);
    sched_lock(); pty_unpin_locked(p); sched_unlock(); assert(pty_live_count()==0);
    struct pty *all[PTY_MAX];
    for (int i=0;i<PTY_MAX;i++) assert(pty_create(&all[i])==0);
    assert(pty_create(&p)==-EMBK_ENFILE && allocated==PTY_MAX);   /* a refused one leaves nothing behind */
    for (int i=0;i<PTY_MAX;i+=2) close_pair(all[i]);
    assert(pty_live_count()==PTY_MAX/2 && allocated==PTY_MAX/2);
    for (int i=0;i<PTY_MAX;i+=2) assert(pty_create(&all[i])==0);   /* freed slots open again */
    for (int i=0;i<PTY_MAX;i++) close_pair(all[i]);
    assert(pty_live_count()==0 && allocated==0);
    /* the output queue takes 32 KiB before a writer must wait */
    assert(pty_create(&p)==0);
    assert(pty_control(p,1,EMBK_TERMINAL_SET_ATTR,&raw)==0);
    static unsigned char big[PTY_OUTPUT];
    memset(big,'o',sizeof big);
    size_t wrote=0;
    assert(pty_write(p,1,big,sizeof big,&wrote,true)==0 && wrote==PTY_OUTPUT-0);
    assert(pty_write(p,1,big,1,&wrote,true)==-EMBK_EAGAIN && pty_avail(p,0)==PTY_OUTPUT);
    close_pair(p);
    assert(pty_live_count()==0 && allocated==0);
    puts("PASS: PTY randomized byte streams, canonical bounds, EOF records, interrupt routing, pinned hangup lifetime, 64 pairs allocated and freed, 32 KiB output queue");
}
