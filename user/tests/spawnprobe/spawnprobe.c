/* Ring-3 spawn adapter acceptance: all children run the installed libc. */
#define _GNU_SOURCE
#include "embk.h"
#include <spawn.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#define SELF "/data/apps/spawnprobe/spawnprobe.elf"
static struct {
    char root[256],nested[256],a[256],b[256],ctor[256],nested_ctor[256],marker[256];
} paths;
static unsigned checks,failures;
#define CHECK(c) do{checks++;if(!(c)){printf("FAIL line %d: %s (errno %d)\n",__LINE__,#c,errno);failures++;}}while(0)
static char ctor_byte;
__attribute__((constructor))static void startup(void) {
    int fd=open("ctor.txt",O_RDONLY);if(fd>=0){(void)read(fd,&ctor_byte,1);close(fd);}
}
static int all_write(int fd,const void *data,size_t n) {
    const char *p=data;while(n){int k=write(fd,p,n);if(k<=0)return -1;p+=k;n-=(size_t)k;}return 0;
}
static int write_file(const char *name,const char *text) {
    int fd=open(name,O_CREAT|O_TRUNC|O_WRONLY,0644);if(fd<0)return -1;
    int rc=all_write(fd,text,strlen(text));close(fd);return rc;
}
struct report {uint32_t pid;int envc;char ctor;char cwd[256],pwd[256],mark[64];};
static int child(int argc,char **argv,char **envp) {
    if(!strcmp(argv[1],"--return"))return argc>2?atoi(argv[2]):42;
    if(!strcmp(argv[1],"--report")) {
        struct report r={0};r.pid=getpid();r.ctor=ctor_byte;
        if(!getcwd(r.cwd,sizeof r.cwd))return 71;
        char context[256]="unchanged";
        if(embk_syscall2(EMBK_SYS_spawn_cwd,(int64_t)(uintptr_t)context,1)!=-34 || strcmp(context,"unchanged"))return 82;
        if(embk_syscall2(EMBK_SYS_spawn_cwd,-1,sizeof context)!=-14)return 83;
        int n=(int)embk_syscall2(EMBK_SYS_spawn_cwd,(int64_t)(uintptr_t)context,sizeof context);
        if(n!=(int)strlen(r.cwd)+1 || strcmp(context,r.cwd))return 84;
        const char *p=getenv("PWD"),*m=getenv("MARK");
        if(p)snprintf(r.pwd,sizeof r.pwd,"%s",p);
        if(m)snprintf(r.mark,sizeof r.mark,"%s",m);
        while(envp && envp[r.envc])r.envc++;
        return all_write(1,&r,sizeof r)?72:0;
    }
    if(!strcmp(argv[1],"--fds")) {
        for(int i=2;i<argc;i++) {
            int value=atoi(argv[i]),fd=value<0?-value:value;
            int flags=fcntl(fd,F_GETFD);
            if(value<0 ? !(flags==-1 && errno==EBADF) : flags!=0)return 73;
        }
        return 0;
    }
    if(!strcmp(argv[1],"--swap")) {
        char a=0,b=0;
        return read(30,&a,1)==1 && read(31,&b,1)==1 && a=='B' && b=='A'?0:74;
    }
    if(!strcmp(argv[1],"--marker"))return argc>2 && !write_file(argv[2],"ran")?0:75;
    if(!strcmp(argv[1],"--producer")) {
        char b[257];for(unsigned i=0;i<sizeof b;i++)b[i]=(char)('a'+i%26);
        for(unsigned n=0;n<257;n++)if(all_write(1,b,sizeof b))return 76;
        return 23;
    }
    if(!strcmp(argv[1],"--upper")) {
        char b[333];int n;while((n=read(0,b,sizeof b))>0) {
            for(int i=0;i<n;i++)if(b[i]>='a' && b[i]<='z')b[i]-=32;
            if(all_write(1,b,(size_t)n))return 77;
        }
        return n<0?78:24;
    }
    if(!strcmp(argv[1],"--closed-stdio")) {
        if(fcntl(0,F_GETFD)!=-1 || errno!=EBADF || fcntl(1,F_GETFD)!=-1 || errno!=EBADF)return 79;
        int fd=open("/dev/null",O_RDWR);if(fd!=0)return 80;
        close(fd);return 0;
    }
    return 81;
}
static void collect(pid_t pid,int expected) {
    int status=-1;CHECK(pid>0);
    if(pid>0){CHECK(waitpid(pid,&status,0)==pid);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==expected);}
}
static int run(const char *mode,const posix_spawn_file_actions_t *fa,char *const envp[],pid_t *pid) {
    char *args[]={"spawnprobe",(char *)mode,!strcmp(mode,"--marker")?paths.marker:NULL,NULL};
    return posix_spawn(pid,SELF,fa,NULL,args,envp);
}
static void attributes(void) {
    posix_spawnattr_t a=NULL;short flags=-1;pid_t group=-1;int policy=-1;
    struct sched_param param={0},got;sigset_t mask,out;memset(&mask,0x5a,sizeof mask);
    CHECK(posix_spawnattr_init(&a)==0);
    CHECK(posix_spawnattr_getflags(&a,&flags)==0 && flags==0);
    CHECK(posix_spawnattr_setflags(&a,POSIX_SPAWN_SETPGROUP)==ENOTSUP);
    CHECK(posix_spawnattr_setflags(&a,0x4000)==EINVAL);
    CHECK(posix_spawnattr_getflags(&a,&flags)==0 && flags==0);
    CHECK(posix_spawnattr_setpgroup(&a,123)==0 && posix_spawnattr_getpgroup(&a,&group)==0 && group==123);
    CHECK(posix_spawnattr_setschedpolicy(&a,7)==0 && posix_spawnattr_getschedpolicy(&a,&policy)==0 && policy==7);
    param.sched_priority=17;
    CHECK(posix_spawnattr_setschedparam(&a,&param)==0 && posix_spawnattr_getschedparam(&a,&got)==0 && got.sched_priority==17);
    CHECK(posix_spawnattr_setsigmask(&a,&mask)==0 && posix_spawnattr_getsigmask(&a,&out)==0 && !memcmp(&mask,&out,sizeof mask));
    CHECK(posix_spawnattr_setsigdefault(&a,&mask)==0 && posix_spawnattr_getsigdefault(&a,&out)==0 && !memcmp(&mask,&out,sizeof mask));
    pid_t pid=0;char *args[]={"spawnprobe","--return","42",NULL};
    errno=EDOM;CHECK(posix_spawn(&pid,SELF,NULL,&a,args,NULL)==0 && errno==EDOM);collect(pid,42);
    CHECK(posix_spawnattr_destroy(&a)==0 && !a);
}
static void descriptors(void) {
    int p[2];CHECK(pipe(p)==0);
    CHECK(fcntl(p[0],F_GETFD)==0 && fcntl(p[1],F_GETFD)==0);
    CHECK(fcntl(p[0],F_SETFD,FD_CLOEXEC)==0 && fcntl(p[0],F_GETFD)==FD_CLOEXEC);
    int d=dup(p[0]);CHECK(d>=0 && fcntl(d,F_GETFD)==0);
    CHECK(dup2(p[0],p[0])==p[0] && fcntl(p[0],F_GETFD)==FD_CLOEXEC);
    CHECK(fcntl(p[0],F_SETFD,0x4000)==-1 && errno==EINVAL && fcntl(p[0],F_GETFD)==FD_CLOEXEC);
    int clo=open("/dev/null",O_RDWR|O_CLOEXEC),file=open(paths.a,O_RDONLY|O_CLOEXEC);
    CHECK(clo>=0 && fcntl(clo,F_GETFD)==FD_CLOEXEC);
    CHECK(file>=0 && fcntl(file,F_GETFD)==FD_CLOEXEC);
    char c0[12],c1[12],cd[12],cc[12],cf[12];
    snprintf(c0,sizeof c0,"-%d",p[0]);snprintf(c1,sizeof c1,"%d",p[1]);snprintf(cd,sizeof cd,"%d",d);
    snprintf(cc,sizeof cc,"-%d",clo);snprintf(cf,sizeof cf,"-%d",file);
    char *args[]={"spawnprobe","--fds",c0,c1,cd,cc,cf,NULL};pid_t pid=0;
    CHECK(posix_spawn(&pid,SELF,NULL,NULL,args,NULL)==0);collect(pid,0);
    posix_spawn_file_actions_t a=NULL;CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,p[0],p[0])==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,file,40)==0);
    snprintf(c0,sizeof c0,"%d",p[0]);char *args2[]={"spawnprobe","--fds",c0,"40",cf,NULL};
    CHECK(posix_spawn(&pid,SELF,&a,NULL,args2,NULL)==0);collect(pid,0);
    CHECK(fcntl(p[0],F_GETFD)==FD_CLOEXEC && fcntl(file,F_GETFD)==FD_CLOEXEC);
    posix_spawn_file_actions_destroy(&a);close(p[0]);close(p[1]);close(d);close(clo);close(file);
    puts("Descriptors: inherited pipes/files, CLOEXEC and dup action semantics verified.");
}
static void directory(void) {
    char parent[256],after[256];CHECK(getcwd(parent,sizeof parent)!=NULL);
    CHECK(chdir(paths.root)==0);CHECK(setenv("PWD","/parent-environment",1)==0);
    posix_spawn_file_actions_t a=NULL;CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_addchdir_np(&a,"nested")==0);
    CHECK(posix_spawn_file_actions_addopen(&a,20,"result.bin",O_WRONLY|O_CREAT|O_TRUNC,0644)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,20,21)==0);
    CHECK(posix_spawn_file_actions_addclose(&a,20)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,21,1)==0);
    CHECK(posix_spawn_file_actions_addclose(&a,21)==0);
    char *env[]={"PWD=/deliberately-different","MARK=exact",NULL};pid_t pid=0;
    errno=EDOM;CHECK(run("--report",&a,env,&pid)==0 && errno==EDOM);collect(pid,0);
    CHECK(getcwd(after,sizeof after)!=NULL && !strcmp(after,paths.root));
    CHECK(!strcmp(getenv("PWD"),"/parent-environment"));
    int fd=open("nested/result.bin",O_RDONLY);struct report r={0};
    CHECK(fd>=0 && read(fd,&r,sizeof r)==sizeof r);if(fd>=0)close(fd);
    CHECK(r.pid==(uint32_t)pid && !strcmp(r.cwd,paths.nested));
    CHECK(!strcmp(r.pwd,"/deliberately-different") && !strcmp(r.mark,"exact") && r.envc==2 && r.ctor=='N');
    posix_spawn_file_actions_destroy(&a);
    /* No PWD or environment is required for an inherited starting directory. */
    int p[2];CHECK(pipe(p)==0);CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,p[1],1)==0);
    CHECK(posix_spawn_file_actions_addclose(&a,p[0])==0 && posix_spawn_file_actions_addclose(&a,p[1])==0);
    CHECK(run("--report",&a,NULL,&pid)==0);close(p[1]);memset(&r,0,sizeof r);
    CHECK(read(p[0],&r,sizeof r)==sizeof r);close(p[0]);collect(pid,0);
    CHECK(!strcmp(r.cwd,paths.root) && !r.pwd[0] && !r.mark[0] && r.envc==0 && r.ctor=='P');
    posix_spawn_file_actions_destroy(&a);CHECK(chdir(parent)==0);
    puts("Directory: ordered relative opens, exact environment and constructor startup verified.");
}
static void ordered(void) {
    posix_spawn_file_actions_t a=NULL;pid_t pid=0;
    CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_addopen(&a,30,paths.a,O_RDONLY,0)==0);
    CHECK(posix_spawn_file_actions_addopen(&a,31,paths.b,O_RDONLY,0)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,30,32)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,31,30)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,32,31)==0);
    CHECK(posix_spawn_file_actions_addclose(&a,32)==0);
    CHECK(run("--swap",&a,NULL,&pid)==0);collect(pid,0);
    CHECK(fcntl(30,F_GETFD)==-1 && errno==EBADF && fcntl(31,F_GETFD)==-1 && errno==EBADF);
    posix_spawn_file_actions_destroy(&a);
    CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_addopen(&a,39,"/dev/null",O_RDONLY|O_CLOEXEC,0)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,39,40)==0);
    char *args[]={"spawnprobe","--fds","-39","40",NULL};
    CHECK(posix_spawn(&pid,SELF,&a,NULL,args,NULL)==0);collect(pid,0);
    posix_spawn_file_actions_destroy(&a);
    CHECK(posix_spawn_file_actions_init(&a)==0);
    for(int i=0;i<61;i++)CHECK(posix_spawn_file_actions_adddup2(&a,0,0)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,0,0)==E2BIG);
    CHECK(run("--return",&a,NULL,&pid)==0);collect(pid,42);
    posix_spawn_file_actions_destroy(&a);
    CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_addclose(&a,0)==0 && posix_spawn_file_actions_addclose(&a,1)==0);
    CHECK(run("--closed-stdio",&a,NULL,&pid)==0);collect(pid,0);
    posix_spawn_file_actions_destroy(&a);
    puts("Actions: file cursor swaps, 61-action ordering and closed stdio verified.");
}
static void paths_and_errors(void) {
    pid_t pid=98765;char *args[]={"spawnprobe","--return",NULL};
    errno=EDOM;CHECK(posix_spawn(&pid,"/does-not-exist",NULL,NULL,args,NULL)==ENOENT && pid==98765 && errno==EDOM);
    CHECK(posix_spawn(&pid,"",NULL,NULL,args,NULL)==ENOENT && pid==98765);
    CHECK(posix_spawn(&pid,paths.a,NULL,NULL,args,NULL)!=0 && pid==98765);
    CHECK(embk_syscall2(EMBK_SYS_spawn_cwd,-1,256)==0); /* legacy startup has no context */
    struct embk_spawn_file_action invalid={0};invalid.kind=EMBK_SPAWN_ACTION_CWD;
    memset(invalid.path,'x',sizeof invalid.path);
    CHECK(embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)SELF,(int64_t)(uintptr_t)args,2,(int64_t)(uintptr_t)&invalid,1,0)==-36);
    CHECK(embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)SELF,(int64_t)(uintptr_t)args,2,-1,1,0)==-14);
    CHECK(embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)SELF,(int64_t)(uintptr_t)args,2,0,65,0)==-7);
    CHECK(embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)SELF,(int64_t)(uintptr_t)args,128,0,0,0)==-7);
    CHECK(embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)SELF,-1,1,0,0,0)==-14);
    struct embk_spawn_file_action wrong_order[2]={{0},{0}};
    wrong_order[0].kind=EMBK_SPAWN_ACTION_KEEP_STATUS;wrong_order[1].kind=EMBK_SPAWN_ACTION_FD_SNAPSHOT;
    CHECK(embk_syscall6(EMBK_SYS_spawn_pid,(int64_t)(uintptr_t)SELF,(int64_t)(uintptr_t)args,2,(int64_t)(uintptr_t)wrong_order,2,0)==-22);
    posix_spawn_file_actions_t a=NULL;CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,64,1)==EBADF);
    CHECK(posix_spawn_file_actions_addclose(&a,-1)==EBADF);
    CHECK(posix_spawn_file_actions_addfchdir_np(&a,0)==ENOTSUP);
    char longpath[300];memset(longpath,'x',sizeof longpath);longpath[299]=0;
    CHECK(posix_spawn_file_actions_addopen(&a,30,longpath,O_RDONLY,0)==ENAMETOOLONG);
    CHECK(posix_spawn_file_actions_addopen(&a,30,paths.a,O_RDONLY,0)==0);
    CHECK(posix_spawn_file_actions_addclose(&a,30)==0 && posix_spawn_file_actions_adddup2(&a,30,1)==0);
    (void)unlink(paths.marker);
    CHECK(run("--marker",&a,NULL,&pid)==EBADF && pid==98765);
    CHECK(access(paths.marker,F_OK)==-1 && errno==ENOENT);
    posix_spawn_file_actions_destroy(&a);CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_addchdir_np(&a,paths.a)==0);
    CHECK(run("--marker",&a,NULL,&pid)==ENOTDIR && pid==98765);
    posix_spawn_file_actions_destroy(&a);CHECK(posix_spawn_file_actions_init(&a)==0);
    CHECK(posix_spawn_file_actions_addchdir_np(&a,"/data/apps/spawnprobe")==0);
    CHECK(posix_spawn(&pid,"spawnprobe.elf",&a,NULL,args,NULL)==0);collect(pid,42);
    char search_path[320];snprintf(search_path,sizeof search_path,"%s:/does-not-exist:.",paths.a);
    CHECK(setenv("PATH",search_path,1)==0);
    char *env[]={"PATH=/wrong-child-path",NULL};
    CHECK(posix_spawnp(&pid,"spawnprobe.elf",&a,NULL,args,env)==0);collect(pid,42);
    CHECK(setenv("PATH",":/does-not-exist",1)==0);
    CHECK(posix_spawnp(&pid,"spawnprobe.elf",&a,NULL,args,NULL)==0);collect(pid,42);
    CHECK(setenv("PATH","/does-not-exist",1)==0);
    CHECK(posix_spawnp(&pid,SELF,NULL,NULL,args,NULL)==0);collect(pid,42);
    pid=98765;CHECK(posix_spawnp(&pid,"missing-command",NULL,NULL,args,NULL)==ENOENT && pid==98765);
    posix_spawn_file_actions_destroy(&a);
    /* Network backings cannot yet share lifetime. Refuse instead of silently
     * closing the parent's socket when a copied child descriptor closes. */
    int sock=socket(AF_INET,SOCK_STREAM,0);CHECK(sock>=0);
    if(sock>=0) {
        CHECK(run("--return",NULL,NULL,&pid)==ENOSYS);
        CHECK(fcntl(sock,F_SETFD,FD_CLOEXEC)==0);
        CHECK(run("--return",NULL,NULL,&pid)==0);collect(pid,42);close(sock);
    }
    puts("Spawn errors: unchanged PID/errno, PATH search, action rollback and socket refusal verified.");
}
static void pressure(void) {
    pid_t pids[32];int count=0;
    for(;count<32;count++){int rc=run("--return",NULL,NULL,&pids[count]);CHECK(rc==0);if(rc)break;}
    (void)unlink(paths.marker);pid_t extra=98765;
    CHECK(run("--marker",NULL,NULL,&extra)==EMFILE && extra==98765);
    embk_sleep_ms(30);CHECK(access(paths.marker,F_OK)==-1 && errno==ENOENT);
    while(count)collect(pids[--count],42);
    CHECK(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);
    puts("Ownership: all 32 child results retained; full table starts no extra child.");
}
static void pipeline(void) {
    int p[2],q[2];CHECK(pipe(p)==0 && pipe(q)==0);
    posix_spawn_file_actions_t a=NULL,b=NULL;pid_t producer=0,upper=0;
    CHECK(posix_spawn_file_actions_init(&a)==0 && posix_spawn_file_actions_init(&b)==0);
    CHECK(posix_spawn_file_actions_adddup2(&a,p[1],1)==0);
    CHECK(posix_spawn_file_actions_adddup2(&b,p[0],0)==0 && posix_spawn_file_actions_adddup2(&b,q[1],1)==0);
    for(int i=0;i<2;i++) {
        CHECK(posix_spawn_file_actions_addclose(&a,p[i])==0 && posix_spawn_file_actions_addclose(&a,q[i])==0);
        CHECK(posix_spawn_file_actions_addclose(&b,p[i])==0 && posix_spawn_file_actions_addclose(&b,q[i])==0);
    }
    CHECK(run("--upper",&b,NULL,&upper)==0 && run("--producer",&a,NULL,&producer)==0);
    close(p[0]);close(p[1]);close(q[1]);
    char data[431];unsigned total=0;int n,valid=1;
    while((n=read(q[0],data,sizeof data))>0) {
        for(int i=0;i<n;i++)if(data[i]!='A'+((total+(unsigned)i)%257)%26)valid=0;
        total+=(unsigned)n;
    }
    close(q[0]);CHECK(n==0 && total==66049 && valid);
    collect(producer,23);collect(upper,24);
    posix_spawn_file_actions_destroy(&a);posix_spawn_file_actions_destroy(&b);
    printf("POSIX pipeline: %u bytes transformed by two spawned children.\n",total);
}
static void fixture_path(char out[256],const char *base,const char *suffix) {
    size_t a=strlen(base),b=strlen(suffix);
    if(a+b>=256){puts("spawnprobe: fixture path too long");exit(1);}
    memcpy(out,base,a);memcpy(out+a,suffix,b+1);
}
int main(int argc,char **argv,char **envp) {
    if(argc>1)return child(argc,argv,envp);
    const char *home=getenv("HOME");if(!home || home[0]!='/')home="/data";
    if(strlen(home)>220){puts("spawnprobe: home path too long");return 1;}
    fixture_path(paths.root,home,"/spawnprobe");
    fixture_path(paths.nested,paths.root,"/nested");
    fixture_path(paths.a,paths.root,"/a.txt");
    fixture_path(paths.b,paths.root,"/b.txt");
    fixture_path(paths.ctor,paths.root,"/ctor.txt");
    fixture_path(paths.nested_ctor,paths.nested,"/ctor.txt");
    fixture_path(paths.marker,paths.root,"/ran.txt");
    (void)mkdir(paths.root,0755);(void)mkdir(paths.nested,0755);
    CHECK(write_file(paths.a,"A")==0 && write_file(paths.b,"B")==0);
    CHECK(write_file(paths.ctor,"P")==0 && write_file(paths.nested_ctor,"N")==0);
    attributes();descriptors();directory();ordered();paths_and_errors();pressure();pipeline();
    printf("spawnprobe: %u/%u checks passed\n",checks-failures,checks);
    return failures?1:0;
}
