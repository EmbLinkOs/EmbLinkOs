#include "document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define PATH_CAP 1024
static unsigned sequence;
uint64_t emdoc_hash(const void *bytes,size_t length) {
    const unsigned char *p=bytes;uint64_t h=UINT64_C(14695981039346656037);
    for(size_t i=0;i<length;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;
}
static int same(const struct emdoc_stamp *a,const struct emdoc_stamp *b) {
    return a->exists==b->exists && (!a->exists || (a->bytes==b->bytes && a->hash==b->hash));
}
int emdoc_probe(const char *path,struct emdoc_stamp *stamp) {
    struct emdoc_stamp s={UINT64_C(14695981039346656037),0,0};
    int fd=open(path,O_RDONLY);
    if(fd<0){if(errno==ENOENT){*stamp=s;return 0;}return -1;}
    struct stat st;int rc=fstat(fd,&st);
    if(!rc && !S_ISREG(st.st_mode)){errno=EINVAL;rc=-1;}
    unsigned char buf[4096];
    while(!rc){ssize_t n=read(fd,buf,sizeof buf);if(n<0){if(errno==EINTR)continue;rc=-1;break;}if(!n)break;
        for(ssize_t i=0;i<n;i++){s.hash^=buf[i];s.hash*=UINT64_C(1099511628211);}s.bytes+=(uint64_t)n;}
    int e=errno;if(close(fd) && !rc){rc=-1;e=errno;}errno=e;
    if(!rc){s.exists=1;*stamp=s;}return rc;
}
int emdoc_read(const char *path,char *text,size_t capacity,struct emdoc_stamp *stamp) {
    if(!text || capacity<1){errno=EINVAL;return -1;}
    int fd=open(path,O_RDONLY);if(fd<0)return -1;
    char *buffer=malloc(capacity);if(!buffer){close(fd);errno=ENOMEM;return -1;}
    struct stat st;int rc=fstat(fd,&st);size_t used=0;
    if(!rc && !S_ISREG(st.st_mode)){errno=EINVAL;rc=-1;}
    while(!rc && used<capacity){ssize_t n=read(fd,buffer+used,capacity-used);
        if(n<0){if(errno==EINTR)continue;rc=-1;break;}if(!n)break;used+=(size_t)n;}
    if(!rc && used==capacity){errno=EFBIG;rc=-1;}
    if(!rc && memchr(buffer,0,used)){errno=EILSEQ;rc=-1;}
    int e=errno;if(close(fd) && !rc){rc=-1;e=errno;}errno=e;
    if(!rc){memcpy(text,buffer,used);text[used]=0;
        if(stamp)*stamp=(struct emdoc_stamp){emdoc_hash(buffer,used),used,1};}
    free(buffer);return rc;
}
int emdoc_save(const char *path,const void *data,size_t length,
               const struct emdoc_stamp *expected,struct emdoc_stamp *saved) {
    if(!path || path[0]!='/' || !strrchr(path,'/')[1] || (!data && length)) {errno=EINVAL;return -1;}
    const char *leaf=strrchr(path,'/');char parent[PATH_CAP],temp[PATH_CAP];
    size_t n=(size_t)(leaf-path);if(!n)n=1;
    if(n>=sizeof parent){errno=ENAMETOOLONG;return -1;}memcpy(parent,path,n);parent[n]=0;
    struct emdoc_stamp old;
    if(emdoc_probe(path,&old))return -1;
    if(expected && !same(expected,&old)){errno=EAGAIN;return -1;}
    struct stat st;mode_t mode=0600;if(old.exists){if(lstat(path,&st))return -1;
        if(!S_ISREG(st.st_mode)){errno=EINVAL;return -1;}mode=st.st_mode&0777;}
    int fd=-1;
    for(unsigned attempt=0;attempt<32;attempt++) {
        int count=snprintf(temp,sizeof temp,"%s%s.emdoc-%lu-%u",parent,!strcmp(parent,"/")?"":"/",(unsigned long)getpid(),++sequence);
        if(count<0 || count>=(int)sizeof temp){errno=ENAMETOOLONG;return -1;}
        fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,mode);if(fd>=0)break;if(errno!=EEXIST)return -1;
    }
    if(fd<0)return -1;
    int rc=0;size_t at=0;
    while(at<length){ssize_t w=write(fd,(const char *)data+at,length-at);
        if(w<0 && errno==EINTR)continue;
        if(w<=0){if(!w)errno=EIO;rc=-1;break;}at+=(size_t)w;}
    if(!rc && fsync(fd))rc=-1;
    int e=errno;if(close(fd) && !rc){rc=-1;e=errno;}errno=e;
    /* Recheck after the potentially slow write, narrowing the conflict window. */
    if(!rc && expected){struct emdoc_stamp now;if(emdoc_probe(path,&now))rc=-1;
        else if(!same(expected,&now)){errno=EAGAIN;rc=-1;}}
    if(!rc && rename(temp,path))rc=-1;
    if(rc){e=errno;unlink(temp);errno=e;return -1;}
    fd=open(parent,O_RDONLY);if(fd<0)return -1;rc=fsync(fd);e=errno;
    if(close(fd) && !rc){rc=-1;e=errno;}errno=e;
    if(!rc && saved)*saved=(struct emdoc_stamp){emdoc_hash(data,length),length,1};
    return rc;
}
/* Fixed header, independent of compiler struct padding. Little endian v1. */
#define DRAFT_HEADER 304
static void put64(unsigned char *p,uint64_t v){for(int i=0;i<8;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint64_t get64(const unsigned char *p){uint64_t v=0;for(int i=7;i>=0;i--)v=(v<<8)|p[i];return v;}
int emdoc_draft_save(const char *file,const struct emdoc_draft *meta,const char *text) {
    size_t n=strlen(text);if(n>UINT32_MAX || !memchr(meta->path,0,sizeof meta->path) || meta->cursor>n){errno=EINVAL;return -1;}
    unsigned char *b=calloc(1,DRAFT_HEADER+n);if(!b)return -1;
    memcpy(b,"EMDRAFT1",8);put64(b+8,n);put64(b+16,meta->cursor);put64(b+24,meta->original.hash);
    put64(b+32,meta->original.bytes);put64(b+40,(uint64_t)meta->original.exists);memcpy(b+48,meta->path,256);
    memcpy(b+DRAFT_HEADER,text,n);
    /* A trailer covers both metadata and text, detecting torn/corrupt files. */
    unsigned char *full=realloc(b,DRAFT_HEADER+n+8);if(!full){free(b);return -1;}b=full;
    put64(b+DRAFT_HEADER+n,emdoc_hash(b,DRAFT_HEADER+n));
    int rc=emdoc_save(file,b,DRAFT_HEADER+n+8,0,0);free(b);return rc;
}
int emdoc_draft_load(const char *file,struct emdoc_draft *meta,char *text,size_t capacity) {
    int fd=open(file,O_RDONLY);if(fd<0)return -1;struct stat st;
    if(fstat(fd,&st) || st.st_size<DRAFT_HEADER+8 || (uint64_t)st.st_size>DRAFT_HEADER+(uint64_t)capacity+7){close(fd);errno=EINVAL;return -1;}
    size_t size=(size_t)st.st_size;unsigned char *b=malloc(size);if(!b){close(fd);return -1;}
    size_t at=0;int rc=0;while(at<size){ssize_t n=read(fd,b+at,size-at);if(n<0 && errno==EINTR)continue;
        if(n<=0){errno=EIO;rc=-1;break;}at+=(size_t)n;}
    int e=errno;close(fd);errno=e;
    if(!rc){uint64_t n=get64(b+8),cursor=get64(b+16),exists=get64(b+40);
        if(memcmp(b,"EMDRAFT1",8) || n!=size-DRAFT_HEADER-8 || n>=capacity || cursor>n || exists>1 ||
           !memchr(b+48,0,256) || (b[48] && b[48]!='/') || memchr(b+DRAFT_HEADER,0,(size_t)n) ||
           get64(b+size-8)!=emdoc_hash(b,size-8)){errno=EINVAL;rc=-1;}
        else{memset(meta,0,sizeof *meta);memcpy(meta->path,b+48,256);meta->cursor=(uint32_t)cursor;
            meta->original=(struct emdoc_stamp){get64(b+24),get64(b+32),(int)exists};
            memcpy(text,b+DRAFT_HEADER,(size_t)n);text[n]=0;}}
    free(b);return rc;
}
