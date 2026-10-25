#define _GNU_SOURCE
#include "document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s (errno %d)\n",__LINE__,#x,errno);exit(1);}count++;}while(0)
static int count;
#ifdef EMDOC_FAULT_TEST
static int fault, writes, syncs;
ssize_t __real_write(int,const void *,size_t);
int __real_fsync(int);
int __real_rename(const char *,const char *);
ssize_t __wrap_write(int fd,const void *data,size_t n) {
    if(fault==1){if(!writes++){errno=EINTR;return -1;}if(n>2)n=2;}
    if(fault==2){if(writes++){errno=ENOSPC;return -1;}if(n>2)n=2;}
    if(fault==5)return 0;
    return __real_write(fd,data,n);
}
int __wrap_fsync(int fd) {
    if((fault==3 && !syncs++) || (fault==6 && syncs++==1)){errno=EIO;return -1;}
    return __real_fsync(fd);
}
int __wrap_rename(const char *a,const char *b) {
    if(fault==4){errno=EIO;return -1;}return __real_rename(a,b);
}
static void failures(const char *file) {
    CHECK(!emdoc_save(file,"original",8,0,0));
    for(int f=2;f<=5;f++) {
        fault=f;writes=syncs=0;
        CHECK(emdoc_save(file,"replacement",11,0,0)<0);
        fault=0;char text[64];CHECK(!emdoc_read(file,text,sizeof text,0) && !strcmp(text,"original"));
    }
    fault=1;writes=0;CHECK(!emdoc_save(file,"short writes",12,0,0));fault=0;
    char text[64];CHECK(!emdoc_read(file,text,sizeof text,0) && !strcmp(text,"short writes"));
    fault=6;syncs=0;CHECK(emdoc_save(file,"replaced, sync failed",21,0,0)<0);fault=0;
    CHECK(!emdoc_read(file,text,sizeof text,0) && !strcmp(text,"replaced, sync failed"));
    CHECK(!unlink(file));
}
#endif

int main(void) {
    char dir[]="/tmp/emdoc-test-XXXXXX",file[256],draft[256],text[128]="unchanged";
    CHECK(mkdtemp(dir));snprintf(file,sizeof file,"%s/report.txt",dir);snprintf(draft,sizeof draft,"%s/draft",dir);
    struct emdoc_stamp missing,first,second;
    CHECK(!emdoc_probe(file,&missing) && !missing.exists);
    CHECK(!emdoc_save(file,"original",8,&missing,&first));
    CHECK(!emdoc_read(file,text,sizeof text,&second) && !strcmp(text,"original"));
    CHECK(first.hash==second.hash && first.bytes==8 && second.exists);
    CHECK(emdoc_save(file,"stale",5,&missing,0)<0 && errno==EAGAIN);
    CHECK(!emdoc_save(file,"external",8,&first,&second));
    CHECK(emdoc_save(file,"lost edit",9,&first,0)<0 && errno==EAGAIN);
    CHECK(!emdoc_read(file,text,sizeof text,0) && !strcmp(text,"external"));
    CHECK(emdoc_save(dir,"bad",3,0,0)<0);
    CHECK(!emdoc_save(file,"binary\0tail",11,&second,&first));
    strcpy(text,"preserved");CHECK(emdoc_read(file,text,sizeof text,0)<0 && errno==EILSEQ && !strcmp(text,"preserved"));
    char big[128];memset(big,'x',sizeof big);CHECK(!emdoc_save(file,big,sizeof big,&first,&second));
    CHECK(emdoc_read(file,text,sizeof text,0)<0 && errno==EFBIG && !strcmp(text,"preserved"));
    CHECK(!emdoc_save(file,"",0,&second,&first));CHECK(!emdoc_read(file,text,sizeof text,0) && !text[0]);
    struct emdoc_draft meta={.cursor=5,.original=first},loaded;strcpy(meta.path,file);
    CHECK(!emdoc_draft_save(draft,&meta,"hello unsaved\n"));
    CHECK(!emdoc_draft_load(draft,&loaded,text,sizeof text) && !strcmp(text,"hello unsaved\n") && !strcmp(loaded.path,file) && loaded.cursor==5 && loaded.original.hash==first.hash);
    int fd=open(draft,O_RDWR);CHECK(fd>=0);CHECK(pwrite(fd,"!",1,310)==1);close(fd);
    strcpy(text,"preserved");CHECK(emdoc_draft_load(draft,&loaded,text,sizeof text)<0 && !strcmp(text,"preserved"));
    CHECK(!unlink(draft));CHECK(!unlink(file));
#ifdef EMDOC_FAULT_TEST
    failures(file);
#endif
    CHECK(!rmdir(dir));
    printf("document-test: %d assertions passed\n",count);return 0;
}
