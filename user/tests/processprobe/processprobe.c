/* Real ring-3 pipe, child-status and wait regressions. No fork emulation. */
#include "embk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define SELF "/data/apps/processprobe/processprobe.elf"
static unsigned checks,failures;
#define CHECK(c) do{checks++;if(!(c)){printf("FAIL line %d: %s (errno %d)\n",__LINE__,#c,errno);failures++;}}while(0)
static int all_write(int fd,const void *data,size_t n) {
    const char *p=data;while(n){int w=write(fd,p,n);if(w<=0)return -1;p+=w;n-=(size_t)w;}return 0;
}
static int spawn(const char *,const char *,int,int,int);
static int child_mode(int argc,char **argv) {
    if(!strcmp(argv[1],"--return"))return argc>2?atoi(argv[2]):0;
    if(!strcmp(argv[1],"--gate")){char b;return read(0,&b,1)<0?80:atoi(argv[2]);}
    if(!strcmp(argv[1],"--identity")){uint32_t pid=getpid();return all_write(1,&pid,sizeof pid)?81:42;}
    if(!strcmp(argv[1],"--producer")) {
        char b[257];for(unsigned i=0;i<sizeof b;i++)b[i]=(char)('a'+i%26);
        for(unsigned n=0;n<257;n++)if(all_write(1,b,sizeof b))return 82;
        return 23;
    }
    if(!strcmp(argv[1],"--upper")) {
        char b[333];int n;while((n=read(0,b,sizeof b))>0){for(int i=0;i<n;i++)if(b[i]>='a' && b[i]<='z')b[i]-=32;if(all_write(1,b,(size_t)n))return 83;}
        return n<0?84:24;
    }
    if(!strcmp(argv[1],"--cancel-wait")) {
        int gate[2];if(pipe(gate))return 88;
        int h=spawn("--gate","0",gate[0],-1,1);close(gate[0]);
        if(h<0 || all_write(1,"ready",5))return 89;
        struct embk_child_result r;
        if(embk_child_control(h,EMBK_CHILD_WAIT_HANDLE,0,&r)!=-125)return 90;
        if(embk_child_control(h,EMBK_CHILD_INFO,0,&r)!=1 || !(r.flags&EMBK_CHILD_RETAINED))return 91;
        if(embk_kill(h))return 92;
        uint64_t until=embk_uptime_ms()+2000;int rc;
        while((rc=embk_child_control(h,EMBK_CHILD_WAIT_HANDLE,EMBK_CHILD_NOHANG,&r))==0 && embk_uptime_ms()<until)embk_yield();
        close(gate[1]);return rc==1 && (r.flags&EMBK_CHILD_KILLED)?0:93;
    }
    if(!strcmp(argv[1],"--closed-stdio")) {
        close(0);close(1);int p[2];if(pipe(p) || p[0]!=0 || p[1]!=1)return 85;
        char b;if(write(p[1],"Z",1)!=1 || read(p[0],&b,1)!=1 || b!='Z')return 86;
        close(p[0]);close(p[1]);return 0;
    }
    return 87;
}
static int spawn(const char *mode,const char *arg,int input,int output,int retain) {
    struct embk_spawn_file_action a[4];memset(a,0,sizeof a);
    for(int i=0;i<3;i++){a[i].kind=EMBK_SPAWN_ACTION_INHERIT_FD;a[i].target_fd=i;a[i].src_obj_handle=i;}
    if(input>=0)a[0].src_obj_handle=input;
    if(output>=0)a[1].src_obj_handle=output;
    a[3].kind=EMBK_SPAWN_ACTION_KEEP_STATUS;
    char *argv[]={"processprobe",(char *)mode,(char *)arg,NULL};
    return (int)embk_spawn(SELF,argv,a,retain?4:3);
}
static struct embk_child_result info(int h) {
    struct embk_child_result result={0};CHECK(h>=0);
    if(h>=0)CHECK(embk_child_control(h,EMBK_CHILD_INFO,0,&result)==1);
    return result;
}
static void collect(int h,int expected) {
    struct embk_child_result r=info(h);int status=0;
    CHECK(waitpid((pid_t)r.pid,&status,0)==(pid_t)r.pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==(expected&255));
}
static void pipes(void) {
    int p[2],q[2];char b[16];struct stat st;
    /* The structured shell may explicitly supply fd 3. Check lowest FREE
     * slots without assuming a child receives only its three stdio fds. */
    int available[64],available_n=0;
    for(int i=0;i<64;i++)if(fcntl(i,F_GETFD)==-1 && errno==EBADF)available[available_n++]=i;
    CHECK(available_n>=2);
    CHECK(pipe(p)==0 && p[0]==available[0] && p[1]==available[1]);
    CHECK(fstat(p[0],&st)==0 && S_ISFIFO(st.st_mode));
    CHECK(fcntl(p[0],F_GETFL)==O_RDONLY && fcntl(p[1],F_GETFL)==O_WRONLY);
    CHECK(write(p[1],"sensor",6)==6 && read(p[0],b,sizeof b)==6 && !memcmp(b,"sensor",6));
    int held=dup(p[1]);CHECK(held>=0);close(p[1]);
    CHECK(embk_fcntl_set_nonblock(p[0],1)==0 && read(p[0],b,1)==-1 && errno==EAGAIN);
    CHECK(fcntl(p[0],F_GETFL)==(O_RDONLY|O_NONBLOCK));
    close(held);CHECK(read(p[0],b,1)==0);close(p[0]);
    CHECK(embk_pipe_fds((int *)(uintptr_t)-1)==-14);
    CHECK(pipe(q)==0 && q[0]==available[0] && q[1]==available[1]);close(q[0]);close(q[1]);
    CHECK(pipe(p)==0);CHECK(embk_fcntl_set_nonblock(p[1],1)==0);
    char fill[1024];memset(fill,'x',sizeof fill);
    CHECK(write(p[1],fill,1023)==1023);
    CHECK(write(p[1],"AB",2)==-1 && errno==EAGAIN && embk_fd_avail(p[0])==1023);
    CHECK(write(p[1],"Z",1)==1 && write(p[1],"Z",1)==-1 && errno==EAGAIN);
    CHECK(read(p[0],fill,sizeof fill)==1024 && fill[1023]=='Z');
    close(p[0]);CHECK(write(p[1],"!",1)==-1 && errno==EPIPE);close(p[1]);
    int legacy[2];
    CHECK(embk_syscall6(EMBK_SYS_pipe,(int64_t)(intptr_t)legacy,0x5a5a,0x1234,0x9876,42,19)==0);
    CHECK(embk_close_handle(legacy[0])==0 && embk_close_handle(legacy[1])==0);
    int all[64],n=0,fd;
    while((fd=dup(0))>=0)all[n++]=fd;
    CHECK(n==available_n && errno==EMFILE);
    int freed=all[--n];close(freed);p[0]=314;p[1]=159;
    CHECK(pipe(p)==-1 && errno==EMFILE && p[0]==314 && p[1]==159);
    fd=dup(0);CHECK(fd==freed);close(fd);
    while(n)close(all[--n]);
    collect(spawn("--closed-stdio",NULL,-1,-1,1),0);
    puts("Pipes: descriptors, EOF, queue pressure and allocation rollback verified.");
}
static void statuses(void) {
    int p[2],status=0;CHECK(pipe(p)==0);
    int h=spawn("--identity",NULL,-1,p[1],1);struct embk_child_result r=info(h);close(p[1]);
    uint32_t actual=0;CHECK(read(p[0],&actual,sizeof actual)==sizeof actual && actual==r.pid);close(p[0]);
    CHECK(r.flags&EMBK_CHILD_RETAINED);
    CHECK(waitpid(r.pid,(int *)(uintptr_t)-1,0)==-1 && errno==EFAULT);
    CHECK(waitpid(r.pid,&status,0)==(pid_t)r.pid && WIFEXITED(status) && WEXITSTATUS(status)==42);
    CHECK(waitpid(r.pid,&status,WNOHANG)==-1 && errno==ECHILD);
    collect(spawn("--return","-1",-1,-1,1),255);
    h=spawn("--return","-10",-1,-1,1);struct embk_child_result signed_result;
    CHECK(embk_child_control(h,EMBK_CHILD_WAIT_HANDLE,0,&signed_result)==1 && signed_result.exit_code==-10 && !(signed_result.flags&EMBK_CHILD_KILLED));
    CHECK(waitpid(getpid(),&status,0)==-1 && errno==ECHILD);
    CHECK(waitpid(0,&status,0)==-1 && errno==ENOSYS);
    CHECK(waitpid(-1,&status,WUNTRACED)==-1 && errno==EINVAL);
    puts("Child status: real PIDs, signed exit values and copyout retry verified.");
}
static void cancellation(void) {
    int ready_pipe[2];CHECK(pipe(ready_pipe)==0);
    int h=spawn("--cancel-wait",NULL,-1,ready_pipe[1],1);struct embk_child_result r=info(h);close(ready_pipe[1]);
    char ready_text[5];CHECK(read(ready_pipe[0],ready_text,sizeof ready_text)==5 && !memcmp(ready_text,"ready",5));close(ready_pipe[0]);
    CHECK(embk_cancel(h)==0);int status=0;
    CHECK(waitpid(r.pid,&status,0)==(pid_t)r.pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void order_and_kill(void) {
    int gate[2],status=0x12345678;CHECK(pipe(gate)==0);
    int slow=spawn("--gate","31",gate[0],-1,1);struct embk_child_result a=info(slow);close(gate[0]);
    CHECK(waitpid(a.pid,&status,WNOHANG)==0 && status==0x12345678);
    int fast=spawn("--return","32",-1,-1,1);struct embk_child_result b=info(fast);
    CHECK(wait(&status)==(pid_t)b.pid && WIFEXITED(status) && WEXITSTATUS(status)==32);
    close(gate[1]);CHECK(waitpid(a.pid,NULL,0)==(pid_t)a.pid);
    CHECK(pipe(gate)==0);int killed=spawn("--gate","0",gate[0],-1,1);struct embk_child_result k=info(killed);close(gate[0]);
    CHECK(embk_kill(killed)==0);CHECK(waitpid(k.pid,&status,0)==(pid_t)k.pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);close(gate[1]);
    CHECK(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    puts("Waits: WNOHANG, out-of-order children, wait-any and hard-kill status verified.");
}
static volatile unsigned ready[2],finished[2];static int wait_handles[2],wait_rc[2];static struct embk_child_result waited[2];
static void waiter(long i) {
    __atomic_store_n(&ready[i],1,__ATOMIC_RELEASE);
    wait_rc[i]=embk_child_control(wait_handles[i],EMBK_CHILD_WAIT_HANDLE,0,&waited[i]);
    __atomic_store_n(&finished[i],1,__ATOMIC_RELEASE);embk_thread_exit(0);
}
static int until(volatile unsigned *flag) {
    uint64_t end=embk_uptime_ms()+2000;
    while(!__atomic_load_n(flag,__ATOMIC_ACQUIRE) && embk_uptime_ms()<end)embk_sleep_ms(5);
    return __atomic_load_n(flag,__ATOMIC_ACQUIRE)!=0;
}
static void competing_waits(int same) {
    int gates[2][2],threads[2];struct embk_child_result children[2];
    memset((void *)ready,0,sizeof ready);memset((void *)finished,0,sizeof finished);
    for(int i=0;i<(same?1:2);i++){CHECK(pipe(gates[i])==0);wait_handles[i]=spawn("--gate",i?"62":"61",gates[i][0],-1,1);children[i]=info(wait_handles[i]);close(gates[i][0]);}
    if(same)wait_handles[1]=wait_handles[0];
    for(int i=0;i<2;i++){threads[i]=(int)embk_thread_create(waiter,i);CHECK(threads[i]>=0);CHECK(until(&ready[i]));}
    embk_sleep_ms(30);
    close(gates[same?0:1][1]);
    if(!same){CHECK(until(&finished[1]));CHECK(!finished[0]);close(gates[0][1]);}
    for(int i=0;i<2;i++){CHECK(until(&finished[i]));CHECK(embk_thread_join(threads[i])==0);}
    if(same)CHECK((wait_rc[0]==1 && wait_rc[1]==-10) || (wait_rc[1]==1 && wait_rc[0]==-10));
    else CHECK(wait_rc[0]==1 && wait_rc[1]==1 && waited[0].pid==children[0].pid && waited[1].pid==children[1].pid);
}
static void retention(void) {
    int handles[32];struct embk_child_result children[32];
    for(int i=0;i<32;i++){handles[i]=spawn("--return","47",-1,-1,1);children[i]=info(handles[i]);}
    int extra=spawn("--return","0",-1,-1,1);CHECK(extra==-24);
    for(int i=31;i>=0;i--){int status;CHECK(waitpid(children[i].pid,&status,0)==(pid_t)children[i].pid && WIFEXITED(status) && WEXITSTATUS(status)==47);}
    collect(spawn("--return","48",-1,-1,1),48);
    puts("Retention: all 32 uncollected child statuses survive handle pressure.");
}
static void pipeline(void) {
    int mid[2],out[2];CHECK(pipe(mid)==0 && pipe(out)==0);
    int producer=spawn("--producer",NULL,-1,mid[1],1);
    int consumer=spawn("--upper",NULL,mid[0],out[1],1);
    close(mid[0]);close(mid[1]);close(out[1]);
    char b[509];unsigned total=0;int count;int valid=1;
    while((count=read(out[0],b,sizeof b))>0){for(int i=0;i<count;i++)if(b[i]!=(char)('A'+(total++%257)%26))valid=0;}
    CHECK(count==0 && total==257*257 && valid);close(out[0]);collect(producer,23);collect(consumer,24);
    printf("Pipeline: %u bytes transformed by two children through bounded pipes.\n",total);
}
int main(int argc,char **argv) {
    if(argc>1)return child_mode(argc,argv);
    puts("EmbLink process compatibility checks");
    pipes();statuses();cancellation();order_and_kill();competing_waits(0);competing_waits(1);retention();pipeline();
    printf("processprobe: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
