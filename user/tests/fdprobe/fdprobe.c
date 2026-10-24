/* Ring-3 open-description, append and socket ownership acceptance. */
#define _GNU_SOURCE
#include "embk.h"
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define SELF "/data/apps/fdprobe/fdprobe.elf"
#define RECORD 257
#define WRITERS 4
#define RECORDS 32
static unsigned checks,failures;
static char root[256],path[256];
#define CHECK(c) do { checks++;if(!(c)){printf("FAIL line %d: %s (errno %d)\n",__LINE__,#c,errno);failures++;} } while(0)
static int putall(int fd,const void *data,size_t n) {
    const char *p=data;while(n){int k=write(fd,p,n);if(k<=0)return -1;p+=k;n-=(size_t)k;}return 0;
}
static int getall(int fd,void *data,size_t n) {
    char *p=data;while(n){int k=read(fd,p,n);if(k<=0)return -1;p+=k;n-=(size_t)k;}return 0;
}
static pid_t child(char **args,int gate,int output) {
    posix_spawn_file_actions_t a={0};pid_t pid=-1;
    int rc=posix_spawn_file_actions_init(&a);
    if(!rc && gate>=0)rc=posix_spawn_file_actions_adddup2(&a,gate,0);
    if(!rc && output>=0)rc=posix_spawn_file_actions_adddup2(&a,output,1);
    if(!rc)rc=posix_spawn(&pid,SELF,&a,NULL,args,NULL);
    posix_spawn_file_actions_destroy(&a);CHECK(!rc);return rc?-1:pid;
}
static void collect(pid_t pid,int expected) {
    int status=0;CHECK(pid>0 && waitpid(pid,&status,0)==pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==expected);
}
static void gated_pipe(int fds[2]) {
    CHECK(pipe(fds)==0);CHECK(fcntl(fds[0],F_SETFD,FD_CLOEXEC)==0 && fcntl(fds[1],F_SETFD,FD_CLOEXEC)==0);
}
static int helper(int argc,char **argv) {
    if(!strcmp(argv[1],"--noop"))return 42;
    if(!strcmp(argv[1],"--flags"))return argc==3 && fcntl(atoi(argv[2]),F_SETFL,O_NONBLOCK|O_APPEND)==0?42:80;
    if(!strcmp(argv[1],"--hold")){if(putall(1,"ready",5))return 81;for(;;)embk_sleep_ms(100);}
    if(!strcmp(argv[1],"--writer")) {
        if(argc!=5)return 82;
        int fd=atoi(argv[2]),tag=atoi(argv[3]);char release;
        if(read(0,&release,1)!=1)return 83;
        if(atoi(argv[4])){fd=open(argv[2],O_WRONLY|O_APPEND);if(fd<0)return 84;}
        char record[RECORD];memset(record,'A'+tag,sizeof record);record[sizeof record-1]='\n';
        for(unsigned i=0;i<RECORDS;i++) {
            memcpy(record,&i,sizeof i);
            if(atoi(argv[4]) && lseek(fd,0,SEEK_SET)!=0)return 85;
            if(write(fd,record,sizeof record)!=sizeof record)return 86;
            embk_yield();
        }
        return 42;
    }
    if(!strcmp(argv[1],"--reader")) {
        if(argc!=3)return 87;
        char release;if(read(0,&release,1)!=1)return 88;
        unsigned record[4];int n;
        while((n=read(atoi(argv[2]),record,sizeof record))>0) {
            if(n!=sizeof record || record[0]>=256 || record[1]!=record[0] || record[2]!=record[0] || record[3]!=record[0])return 89;
            if(putall(1,record,sizeof record[0]))return 90;
            embk_yield();
        }
        return n<0?91:42;
    }
    if(!strcmp(argv[1],"--tcp-child")) {
        int fd=atoi(argv[2]);char reply[10];
        return putall(fd,"child-net\n",10) || getall(fd,reply,10) || memcmp(reply,"child-net\n",10)?92:42;
    }
    return 93;
}
static void flags_for(int fd) {
    CHECK(fd>=0);if(fd<0)return;
    int acc=fcntl(fd,F_GETFL)&O_ACCMODE,copy=dup(fd);CHECK(copy>=0);
    CHECK(fcntl(fd,F_SETFD,FD_CLOEXEC)==0);
    CHECK(fcntl(copy,F_SETFL,O_NONBLOCK|O_APPEND)==0);
    CHECK(fcntl(fd,F_GETFL)==(acc|O_NONBLOCK|O_APPEND));
    CHECK(fcntl(fd,F_GETFD)==FD_CLOEXEC && fcntl(copy,F_GETFD)==0);
    CHECK(embk_fcntl_set_nonblock(fd,0)==0 && fcntl(copy,F_GETFL)==(acc|O_APPEND));
    CHECK(fcntl(fd,F_SETFL,O_RDWR)==0 && fcntl(copy,F_GETFL)==acc);
    int high=fcntl(copy,F_DUPFD,48);CHECK(high>=48);
    CHECK(fcntl(high,F_SETFL,O_NONBLOCK)==0 && fcntl(fd,F_GETFL)==(acc|O_NONBLOCK));
    CHECK(fcntl(fd,F_SETFL,O_TRUNC)==-1 && errno==EINVAL && fcntl(high,F_GETFL)==(acc|O_NONBLOCK));
    CHECK(embk_syscall3(EMBK_SYS_fcntl,fd,6,16)==-22);
    CHECK(fcntl(copy,F_SETFL,0)==0 && fcntl(fd,F_GETFL)==acc);
    close(high);close(copy);close(fd);
}
static void status_flags(void) {
    flags_for(open(path,O_CREAT|O_TRUNC|O_RDWR,0644));
    flags_for(open("/dev/null",O_RDWR));
    int p[2];CHECK(pipe(p)==0);flags_for(p[0]);CHECK(fcntl(p[1],F_GETFL)==O_WRONLY);close(p[1]);
    CHECK(embk_openpty(p)==0);flags_for(p[0]);CHECK(fcntl(p[1],F_GETFL)==O_RDWR);close(p[1]);
    flags_for(socket(AF_INET,SOCK_STREAM,0));flags_for(socket(AF_INET,SOCK_DGRAM,0));
    int a=open(path,O_RDWR),b=open(path,O_RDWR);CHECK(a>=0 && b>=0);
    CHECK(fcntl(a,F_SETFL,O_NONBLOCK)==0 && fcntl(b,F_GETFL)==O_RDWR);
    char number[16];snprintf(number,sizeof number,"%d",a);
    char *args[]={"fdprobe","--flags",number,NULL};collect(child(args,-1,-1),42);
    CHECK(fcntl(a,F_GETFL)==(O_RDWR|O_NONBLOCK|O_APPEND) && fcntl(b,F_GETFL)==O_RDWR);
    close(a);close(b);
    if(!failures)puts("Status: shared across dup/spawn; CLOEXEC, separate opens and pipe/PTY ends stay independent.");
}
static void append(void) {
    int a=open(path,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(a>=0 && write(a,"seed",4)==4);
    int b=open(path,O_RDWR|O_APPEND);CHECK(b>=0 && lseek(b,0,SEEK_CUR)==0);
    CHECK(write(a,"X",1)==1);CHECK(lseek(b,0,SEEK_SET)==0 && write(b,"Y",1)==1 && lseek(b,0,SEEK_CUR)==6);
    char data[16]={0};CHECK(lseek(a,0,SEEK_SET)==0 && read(a,data,sizeof data)==6 && !memcmp(data,"seedXY",6));
    CHECK(lseek(b,0,SEEK_SET)==0 && write(b,"",0)==0 && lseek(b,0,SEEK_CUR)==0);
    CHECK(fcntl(b,F_SETFL,0)==0 && write(b,"!",1)==1);
    CHECK(fcntl(b,F_SETFL,O_APPEND)==0 && lseek(b,0,SEEK_SET)==0 && write(b,"Z",1)==1);
    CHECK(lseek(a,0,SEEK_SET)==0 && read(a,data,sizeof data)==7 && !memcmp(data,"!eedXYZ",7));close(a);close(b);
}
static void parallel_writers(int independent) {
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);int gate[2];gated_pipe(gate);
    char number[16];snprintf(number,sizeof number,"%d",fd);pid_t pids[WRITERS];
    for(int i=0;i<WRITERS;i++) {
        char tag[4];snprintf(tag,sizeof tag,"%d",i);
        char *args[]={"fdprobe","--writer",independent?path:number,tag,independent?"1":"0",NULL};
        pids[i]=child(args,gate[0],-1);
    }
    CHECK(write(gate[1],"abcd",WRITERS)==WRITERS);close(gate[0]);close(gate[1]);
    for(int i=0;i<WRITERS;i++)collect(pids[i],42);
    struct stat st;CHECK(fstat(fd,&st)==0 && st.st_size==WRITERS*RECORDS*RECORD);
    CHECK(lseek(fd,0,SEEK_SET)==0);unsigned char seen[WRITERS][RECORDS]={0};char record[RECORD];int n;unsigned total=0;int valid=1;
    while((n=read(fd,record,sizeof record))>0) {
        unsigned index=0;memcpy(&index,record,sizeof index);int tag=record[4]-'A';
        if(n!=sizeof record || tag<0 || tag>=WRITERS || index>=RECORDS || record[RECORD-1]!='\n'){valid=0;break;}
        if(seen[tag][index]++)valid=0;
        for(unsigned j=4;j<RECORD-1;j++)if(record[j]!='A'+tag)valid=0;
        total++;
    }
    CHECK(valid && n==0 && total==WRITERS*RECORDS);close(fd);
}
static void parallel_readers(void) {
    int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0644);CHECK(fd>=0);
    for(unsigned i=0;i<256;i++){unsigned record[4]={i,i,i,i};if(putall(fd,record,sizeof record)){CHECK(0);break;}}
    CHECK(lseek(fd,0,SEEK_SET)==0);int gate[2],out[2];gated_pipe(gate);gated_pipe(out);
    char number[16];snprintf(number,sizeof number,"%d",fd);char *args[]={"fdprobe","--reader",number,NULL};pid_t pids[WRITERS];
    for(int i=0;i<WRITERS;i++)pids[i]=child(args,gate[0],out[1]);
    close(out[1]);CHECK(write(gate[1],"abcd",WRITERS)==WRITERS);close(gate[0]);close(gate[1]);
    unsigned index,seen[256]={0},total=0;int valid=1;
    while(read(out[0],&index,sizeof index)==sizeof index){if(index>=256 || seen[index]++)valid=0;total++;}
    CHECK(valid && total==256);for(int i=0;i<WRITERS;i++)collect(pids[i],42);close(out[0]);close(fd);
    if(!failures)puts("Files: append selects EOF per write; concurrent readers, shared cursors and independent append writers verified.");
}
static struct sockaddr_in address(unsigned port) {
    struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons(port);return a;
}
static void socket_ownership(void) {
    int u=socket(AF_INET,SOCK_DGRAM,0),copy=dup(u),other=socket(AF_INET,SOCK_DGRAM,0);CHECK(u>=0 && copy>=0 && other>=0);
    struct sockaddr_in a=address(39127);CHECK(bind(u,(void *)&a,sizeof a)==0);close(u);
    CHECK(bind(other,(void *)&a,sizeof a)==-1);
    char *args[]={"fdprobe","--noop",NULL};collect(child(args,-1,-1),42);
    CHECK(bind(other,(void *)&a,sizeof a)==-1);close(copy);
    CHECK(bind(other,(void *)&a,sizeof a)==0);close(other);
    u=socket(AF_INET,SOCK_DGRAM,0);copy=dup(u);CHECK(u>=0 && copy>=0);
    CHECK(fcntl(copy,F_SETFL,O_NONBLOCK)==0);
    char byte;uint64_t started=embk_uptime_ms();
    CHECK(read(u,&byte,1)==-1 && errno==EAGAIN);
    CHECK(recvfrom(copy,&byte,1,0,NULL,NULL)==-1 && errno==EAGAIN);
    CHECK(embk_uptime_ms()-started<500 && !(embk_fd_poll(u,EMBK_POLLIN)&EMBK_POLLIN));close(u);close(copy);
    int s=socket(AF_INET,SOCK_STREAM,0),alias=dup(s);CHECK(s>=0 && alias>=0);
    a=address(39128);CHECK(bind(alias,(void *)&a,sizeof a)==0 && listen(s,4)==0);
    CHECK(fcntl(alias,F_SETFL,O_NONBLOCK)==0);
    started=embk_uptime_ms();CHECK(accept(s,NULL,NULL)==-1 && errno==EAGAIN && embk_uptime_ms()-started<500);
    CHECK(!(embk_fd_poll(alias,EMBK_POLLIN)&EMBK_POLLNVAL));close(s);
    collect(child(args,-1,-1),42);CHECK(!(embk_fd_poll(alias,EMBK_POLLIN)&EMBK_POLLNVAL));close(alias);
    for(int round=0;round<40;round++){u=socket(AF_INET,SOCK_DGRAM,0);copy=dup(u);CHECK(u>=0 && copy>=0);close(u);close(copy);}
    if(!failures)puts("Sockets: TCP/UDP identity survives duplicate close and child exit; last close releases ownership.");
}
static void network(void) {
    struct sockaddr_in a=address(39381);a.sin_addr.s_addr=htonl(0x0a000202);
    int s=socket(AF_INET,SOCK_STREAM,0),alias=dup(s);CHECK(s>=0 && alias>=0);
    CHECK(connect(alias,(void *)&a,sizeof a)==0);close(alias);
    char number[16],reply[16];snprintf(number,sizeof number,"%d",s);
    char *args[]={"fdprobe","--tcp-child",number,NULL};collect(child(args,-1,-1),42);
    CHECK(putall(s,"parent-net\n",11)==0 && getall(s,reply,11)==0 && !memcmp(reply,"parent-net\n",11));
    int ready[2];gated_pipe(ready);char *hold[]={"fdprobe","--hold",NULL};pid_t pid=child(hold,-1,ready[1]);close(ready[1]);
    CHECK(getall(ready[0],reply,5)==0 && !memcmp(reply,"ready",5));close(ready[0]);
    int handle=-1;for(int h=0;h<32;h++){struct embk_child_result info;if(embk_child_control(h,EMBK_CHILD_INFO,0,&info)==1 && info.pid==(unsigned)pid)handle=h;}
    CHECK(handle>=0 && embk_kill(handle)==0);
    int status=0;CHECK(waitpid(pid,&status,0)==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    CHECK(putall(s,"after-kill\n",11)==0 && getall(s,reply,11)==0 && !memcmp(reply,"after-kill\n",11));close(s);
    int u=socket(AF_INET,SOCK_DGRAM,0);alias=dup(u);CHECK(u>=0 && alias>=0);close(u);a=address(39382);a.sin_addr.s_addr=htonl(0x0a000202);
    CHECK(sendto(alias,"datagram",8,0,(void *)&a,sizeof a)==8);
    CHECK(recvfrom(alias,reply,sizeof reply,0,NULL,NULL)==8 && !memcmp(reply,"datagram",8));
    char oversized[1473]={0};CHECK(sendto(alias,oversized,sizeof oversized,0,(void *)&a,sizeof a)==-1 && errno==EMSGSIZE);
    CHECK(embk_net_sendto(alias,0x0a000202,39382,oversized,1UL<<32)==-90);
    CHECK(sendto(alias,"",0,0,(void *)&a,sizeof a)==0 && recvfrom(alias,reply,sizeof reply,0,NULL,NULL)==0);close(alias);
    if(!failures)puts("Network: connected TCP transfer, child exit/kill and UDP duplicate traffic verified with local echo fixtures.");
}
int main(int argc,char **argv) {
    if(argc>1 && strcmp(argv[1],"--network"))return helper(argc,argv);
    if(argc>1)network();else {
        const char *home=getenv("HOME");if(!home || home[0]!='/')home="/data";
        if(strlen(home)>210)return 1;
        snprintf(root,sizeof root,"%s/fdprobe",home);(void)mkdir(root,0755);
        snprintf(path,sizeof path,"%.240s/records.bin",root);
        status_flags();append();parallel_writers(0);parallel_writers(1);parallel_readers();socket_ownership();(void)unlink(path);
    }
    printf("fdprobe: %u/%u checks passed\n",checks-failures,checks);return failures?1:0;
}
