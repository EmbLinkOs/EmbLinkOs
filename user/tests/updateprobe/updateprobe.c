/* Native regression checks for update authority and immutable deployment pages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "embk.h"
static unsigned checks,failures;
static void check(int ok,const char *name) {checks++;if(!ok){failures++;printf("FAIL updateprobe: %s (errno %d)\n",name,errno);}}
static const char *self="/data/apps/updateprobe/updateprobe.elf";
static void private_mapping(int fd,const char *name) {
    unsigned char original=0;check(pread(fd,&original,1,0)==1,"read original byte");
    unsigned char *p=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);
    check(p!=MAP_FAILED,name);if(p==MAP_FAILED)return;
    check(p[0]==original,"private read starts at original byte");
    if(mprotect(p,4096,PROT_READ|PROT_WRITE)){check(0,"private mprotect enables COW");munmap(p,4096);return;}
    p[0]=original^0x55;unsigned char actual=0;
    check(pread(fd,&actual,1,0)==1 && actual==original,"COW after mprotect preserves file cache");
    check(mprotect(p,4096,PROT_READ)==0,"private downgrade");
    check(mprotect(p,4096,PROT_READ|PROT_WRITE)==0,"private upgrade keeps private copy");
    p[0]=original^0x33;
    check(pread(fd,&actual,1,0)==1 && actual==original,"repeated mprotect cannot dirty original");
    check(munmap(p,4096)==0,"unmap private file");
}
static int guest(void) {
    struct embk_update_info info;
    check(!embk_update(EMBK_UPDATE_STATUS,&info,sizeof info),"public status available");
    check(!(info.flags&EMBK_UPDATE_ADMIN),"forged USER does not confer administrator role");
    check(embk_update(EMBK_UPDATE_CHECK,NULL,0)==-1,"ordinary account cannot request machine update");
    check(embk_update(EMBK_UPDATE_ABORT,NULL,0)==-1,"ordinary account cannot touch worker transaction");
    struct embk_spawn_file_action action;embk_action_new_session(&action,"administrator");action.flags=EMBK_SPAWN_SESSION_ADMIN;
    char *argv[]={(char *)self,"--guest",NULL};
    check(embk_spawn(self,argv,&action,1)==-1,"session cannot mint administrator identity");
    return failures?1:0;
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--guest")) {
        int rc=guest();printf("updateprobe guest: %u/%u checks passed\n",checks-failures,checks);return rc;
    }
    struct embk_update_info info;check(!embk_update(EMBK_UPDATE_STATUS,&info,sizeof info),"update ABI available");
    check(info.flags&EMBK_UPDATE_MANAGED,"managed disk detected");
    check(embk_update(EMBK_UPDATE_ABORT,NULL,0)==-1,"even privileged non-worker PID cannot abort transaction");
    int immutable=open("/system/bin/home.elf",O_WRONLY);
    check(immutable<0 && errno==EROFS,"physical OS volume refuses writable open");if(immutable>=0)close(immutable);
    immutable=open("/system/bin/home.elf",O_RDONLY);check(immutable>=0,"read sealed executable");
    if(immutable>=0){private_mapping(immutable,"private immutable executable mapping");close(immutable);}
    const char *path="/data/tmp/update-map-probe";int fd=open(path,O_RDWR|O_CREAT|O_TRUNC,0600);
    check(fd>=0,"open shared-data scratch");
    if(fd>=0) {
        unsigned char buffer[4096];memset(buffer,'A',sizeof buffer);
        check(write(fd,buffer,sizeof buffer)==sizeof buffer,"seed data file");check(!fsync(fd),"sync data file");close(fd);
        fd=open(path,O_RDONLY);check(fd>=0,"read-only data descriptor");
        if(fd>=0) {
            errno=0;void *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
            check(p==MAP_FAILED && errno==EACCES,"shared write requires writable descriptor");if(p!=MAP_FAILED)munmap(p,4096);
            p=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0);check(p!=MAP_FAILED,"shared read-only mapping");
            if(p!=MAP_FAILED) {
                close(fd);fd=-1;errno=0;
                check(mprotect(p,4096,PROT_READ|PROT_WRITE)<0 && errno==EACCES,"mprotect retains descriptor authority after close");munmap(p,4096);
            }
            if(fd>=0)close(fd);
        }
        fd=open(path,O_RDONLY);if(fd>=0){private_mapping(fd,"private shared-data mapping");close(fd);}
        fd=open(path,O_RDWR);
        if(fd>=0) {
            unsigned char *p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
            check(p!=MAP_FAILED,"shared writable mapping still works");
            if(p!=MAP_FAILED){p[0]='B';unsigned char actual=0;check(pread(fd,&actual,1,0)==1 && actual=='B',"writable shared cache remains coherent");check(!fsync(fd),"sync shared writer");munmap(p,4096);}close(fd);
        }
        unlink(path);
    }
    struct embk_spawn_file_action actions[4];
    embk_action_set_caps(&actions[0],EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)|EMBK_CAP_BIT(EMBK_CAP_POWER));
    embk_action_ns_bind(&actions[1],"/system",EMBK_NS_RO);embk_action_ns_bind(&actions[2],"/data/apps",EMBK_NS_RO);
    embk_action_new_session(&actions[3],"updateguest");
    char *av[]={(char *)self,"--guest",NULL},*env[]={"USER=administrator","HOME=/",NULL};
    int h=(int)embk_spawn_env(self,av,env,actions,4);check(h>=0,"spawn ordinary-account witness");
    if(h>=0)check(embk_wait(h)==0,"ordinary account update boundaries");
    printf("updateprobe: %u/%u checks passed\n",checks-failures,checks);return failures?1:0;
}
