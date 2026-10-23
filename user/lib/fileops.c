#define _GNU_SOURCE
#include "fileops.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef EMBK_FILEOPS_HOST
#include <sys/syscall.h>
static int no_replace(const char *a,const char *b) {return (int)syscall(SYS_renameat2,AT_FDCWD,a,AT_FDCWD,b,1);}
#else
#include "embk.h"
static int no_replace(const char *a,const char *b) {
    int rc=(int)embk_syscall2(EMBK_SYS_rename_noreplace,(int64_t)(uintptr_t)a,(int64_t)(uintptr_t)b);
    if(rc<0) {errno=-rc;return -1;}return 0;
}
#endif
#define PATH_CAP 1024
static unsigned sequence;
int embk_file_valid_name(const char *s) {
    size_t n=strlen(s);if(!n || n>255 || !strcmp(s,".") || !strcmp(s,"..")) return 0;
    for(size_t i=0;i<n;i++) if((unsigned char)s[i]<32 || s[i]=='/' || (unsigned char)s[i]==127) return 0;
    return 1;
}
static int normalized(const char *src,char *dst) {
    if(!src || src[0]!='/') {errno=EINVAL;return -1;}
    size_t at=1;dst[0]='/';
    for(size_t i=1;src[i];) {
        while(src[i]=='/')i++;
        size_t start=i;while(src[i] && src[i]!='/')i++;
        size_t n=i-start;if(!n) break;
        if(n==1 && src[start]=='.')continue;
        if(n==2 && src[start]=='.' && src[start+1]=='.') {
            while(at>1 && dst[at-1]!='/')at--;
            if(at>1)at--;
            continue;
        }
        if(n>255 || at+n+2>=PATH_CAP) {errno=ENAMETOOLONG;return -1;}
        if(at>1)dst[at++]='/';
        memcpy(dst+at,src+start,n);at+=n;
    }
    dst[at]=0;return 0;
}
static int under(const char *p,const char *base) {
    size_t n=strlen(base);return !strncmp(p,base,n) && (!p[n] || p[n]=='/');
}
static int child(char *out,const char *base,const char *name) {
    if(!embk_file_valid_name(name)) {errno=EINVAL;return -1;}
    int n=snprintf(out,PATH_CAP,"%s%s%s",base,!strcmp(base,"/")?"":"/",name);
    if(n<0 || n>=PATH_CAP) {errno=ENAMETOOLONG;return -1;}return 0;
}
static int sync_path(const char *path) {
    int fd=open(path,O_RDONLY);if(fd<0)return -1;
    int rc=fsync(fd),saved=errno;close(fd);errno=saved;return rc;
}
static int sync_parent(const char *path) {
    char parent[PATH_CAP];snprintf(parent,sizeof parent,"%s",path);
    char *slash=strrchr(parent,'/');if(slash==parent)slash[1]=0;else if(slash)*slash=0;
    return sync_path(parent);
}
static int remove_tree(const char *p,unsigned depth) {
    if(depth>16) {errno=ELOOP;return -1;}
    struct stat st;if(lstat(p,&st))return -1;
    if(!S_ISDIR(st.st_mode))return unlink(p);
    DIR *d=opendir(p);if(!d)return -1;int rc=0;struct dirent *e;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char path[PATH_CAP];if(child(path,p,e->d_name) || remove_tree(path,depth+1)) {rc=-1;break;}
    }
    int saved=errno;closedir(d);errno=saved;return rc?rc:rmdir(p);
}
struct copy {uint8_t data[32768];unsigned entries;int root_created;};
static int copy_item(struct copy *c,const char *src,const char *dst,unsigned depth) {
    if(depth>16 || ++c->entries>100000) {errno=ELOOP;return -1;}
    struct stat st;if(lstat(src,&st))return -1;
    if(S_ISDIR(st.st_mode)) {
        if(mkdir(dst,0700))return -1;
        if(!depth)c->root_created=1;
        DIR *d=opendir(src);if(!d)return -1;int rc=0;struct dirent *e;
        while((e=readdir(d))) {
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            char a[PATH_CAP],b[PATH_CAP];
            if(child(a,src,e->d_name)||child(b,dst,e->d_name)||copy_item(c,a,b,depth+1)) {rc=-1;break;}
        }
        int saved=errno;closedir(d);errno=saved;
        if(!rc && (chmod(dst,st.st_mode&0777) || sync_path(dst)))rc=-1;
        return rc;
    }
    if(!S_ISREG(st.st_mode)) {errno=ENOTSUP;return -1;}
    int in=open(src,O_RDONLY);if(in<0)return -1;
    int out=open(dst,O_WRONLY|O_CREAT|O_EXCL,st.st_mode&0777);
    if(out<0) {int saved=errno;close(in);errno=saved;return -1;}
    if(!depth)c->root_created=1;
    int rc=0;uint64_t total=0;
    for(;;) {
        ssize_t n=read(in,c->data,sizeof c->data);
        if(n<0){rc=-1;break;}if(!n)break;
        for(ssize_t off=0;off<n;) {ssize_t wrote=write(out,c->data+off,(size_t)(n-off));
            if(wrote<=0){rc=-1;break;}off+=wrote;}
        if(rc)break;
        total+=(uint64_t)n;
    }
    if(!rc && total!=(uint64_t)st.st_size) {errno=EAGAIN;rc=-1;}
    if(!rc && fsync(out))rc=-1;
    int saved=errno;close(in);if(close(out) && !rc){saved=errno;rc=-1;}errno=saved;return rc;
}
int embk_file_copy(const char *source,const char *destination) {
    char src[PATH_CAP],dst[PATH_CAP],temp[PATH_CAP];
    if(normalized(source,src)||normalized(destination,dst))return -1;
    if(!strcmp(src,"/") || under(dst,src)) {errno=EINVAL;return -1;}
    struct stat st;if(!lstat(dst,&st)){errno=EEXIST;return -1;}if(errno!=ENOENT)return -1;
    int n=snprintf(temp,sizeof temp,"%.*s.emblink-copy-%lu-%u",(int)(strrchr(dst,'/')-dst+1),dst,(unsigned long)getpid(),++sequence);
    if(n<0||n>=(int)sizeof temp){errno=ENAMETOOLONG;return -1;}
    struct copy *c=calloc(1,sizeof *c);if(!c)return -1;
    int rc=copy_item(c,src,temp,0),owned=c->root_created;free(c);
    if(!rc)rc=no_replace(temp,dst);
    if(rc){int saved=errno;if(owned)remove_tree(temp,0);errno=saved;return -1;}
    return sync_parent(dst);
}
int embk_file_move(const char *source,const char *destination) {
    char src[PATH_CAP],dst[PATH_CAP];
    if(normalized(source,src)||normalized(destination,dst))return -1;
    if(!strcmp(src,"/") || under(dst,src)) {errno=EINVAL;return -1;}
    if(no_replace(src,dst))return -1;
    int a=sync_parent(src),saved=errno,b=sync_parent(dst);if(a)errno=saved;return a?a:b;
}
int embk_file_trash(const char *source,const char *home,char *record,size_t cap) {
    char src[PATH_CAP],h[PATH_CAP],trash[PATH_CAP],box[PATH_CAP],item[PATH_CAP],meta[PATH_CAP];
    if(normalized(source,src)||normalized(home,h)||child(trash,h,"Trash"))return -1;
    if(!under(src,h) || !strcmp(src,h) || under(src,trash)) {errno=EINVAL;return -1;}
    struct stat st;if(lstat(trash,&st) || !S_ISDIR(st.st_mode)) {errno=ENOTDIR;return -1;}
    const char *name=strrchr(src,'/');if(!name || !name[1]) {errno=EINVAL;return -1;}
    int made=0;
    for(unsigned attempt=0;attempt<32;attempt++) {
        char leaf[256];snprintf(leaf,sizeof leaf,"%.180s-%lu-%u.trash",name+1,(unsigned long)getpid(),++sequence);
        if(child(box,trash,leaf))return -1;
        if(strlen(box)+1>cap) {errno=ENAMETOOLONG;return -1;}
        if(!mkdir(box,0700)){made=1;break;}if(errno!=EEXIST)return -1;
    }
    if(!made){errno=EEXIST;return -1;}
    if(child(item,box,"item")||child(meta,box,"original.path")) {rmdir(box);return -1;}
    int fd=open(meta,O_WRONLY|O_CREAT|O_EXCL,0600);int rc=fd<0?-1:0;
    size_t length=strlen(src);
    if(!rc && (write(fd,src,length)!=(ssize_t)length || fsync(fd)))rc=-1;
    int saved=errno;if(fd>=0 && close(fd) && !rc){rc=-1;saved=errno;}errno=saved;
    if(!rc)rc=sync_path(box);
    if(!rc)rc=no_replace(src,item);
    if(rc){saved=errno;unlink(meta);rmdir(box);errno=saved;return -1;}
    memcpy(record,box,strlen(box)+1);
    if(sync_parent(src)||sync_path(box)||sync_path(trash))return -1;
    return 0;
}
int embk_file_restore(const char *record,const char *home) {
    char box[PATH_CAP],h[PATH_CAP],trash[PATH_CAP],item[PATH_CAP],meta[PATH_CAP],original[PATH_CAP],dst[PATH_CAP];
    if(normalized(record,box)||normalized(home,h)||child(trash,h,"Trash"))return -1;
    if(!under(box,trash)||!strcmp(box,trash)||child(item,box,"item")||child(meta,box,"original.path")) {errno=EINVAL;return -1;}
    struct stat st;if(lstat(box,&st)||!S_ISDIR(st.st_mode)||lstat(meta,&st)||!S_ISREG(st.st_mode)) {errno=EINVAL;return -1;}
    int fd=open(meta,O_RDONLY);if(fd<0)return -1;
    ssize_t n=read(fd,original,sizeof original-1);char extra;ssize_t more=read(fd,&extra,1);close(fd);
    if(n<=0 || more!=0 || memchr(original,0,(size_t)n)) {errno=EINVAL;return -1;}
    original[n]=0;
    if(normalized(original,dst)||!under(dst,h)||!strcmp(dst,h)||under(dst,trash)) {errno=EINVAL;return -1;}
    if(no_replace(item,dst))return -1;
    if(sync_parent(dst))return -1;
    if(unlink(meta)||rmdir(box)||sync_path(trash))return -1;
    return 0;
}
