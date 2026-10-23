#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../user/lib/fileops.h"
static int short_writes,reads,fail_read,syncs,fail_sync;
ssize_t __real_read(int,void *,size_t);
ssize_t __real_write(int,const void *,size_t);
int __real_fsync(int);
ssize_t __wrap_read(int f,void *p,size_t n){if(fail_read && ++reads==fail_read){errno=EIO;return -1;}return __real_read(f,p,n);}
ssize_t __wrap_write(int f,const void *p,size_t n){return __real_write(f,p,short_writes && n>7?7:n);}
int __wrap_fsync(int f){if(fail_sync && ++syncs==fail_sync){errno=EIO;return -1;}return __real_fsync(f);}
static void path(char *p,const char *a,const char *b){size_t n=strlen(a);assert(n+strlen(b)+2<1024);memcpy(p,a,n);p[n++]='/';strcpy(p+n,b);}
static unsigned char data[70001],got[70001];
static void fixture(const char *p){int f=open(p,O_CREAT|O_EXCL|O_WRONLY,0600);assert(f>=0);assert(write(f,data,sizeof data)==sizeof data);assert(!close(f));}
static void same(const char *p){int f=open(p,O_RDONLY);assert(f>=0);size_t off=0;while(off<sizeof got){ssize_t n=read(f,got+off,sizeof got-off);assert(n>0);off+=n;}assert(!memcmp(got,data,sizeof data));close(f);}
int main(void){
    for(unsigned i=0;i<sizeof data;i++)data[i]=(unsigned char)(i*11+7);
    char home[]="/tmp/emblink-fileops-XXXXXX";assert(mkdtemp(home));
    char src[1024],dst[1024],trash[1024],record[1024],folder[1024],sub[1024],copied[1024],destfile[1024];
    path(src,home,"original");path(dst,home,"copy");path(trash,home,"Trash");assert(!mkdir(trash,0700));fixture(src);
    assert(embk_file_copy(src,src)<0);same(src);
    short_writes=1;assert(!embk_file_copy(src,dst));short_writes=0;same(dst);same(src);
    assert(embk_file_copy(src,dst)<0 && errno==EEXIST);same(dst);
    unlink(dst);fail_read=2;reads=0;assert(embk_file_copy(src,dst)<0);fail_read=0;
    assert(access(dst,F_OK)<0);same(src);
    fail_sync=1;syncs=0;assert(embk_file_copy(src,dst)<0);fail_sync=0;assert(access(dst,F_OK)<0);
    path(folder,home,"folder");assert(!mkdir(folder,0700));path(sub,folder,"sub");assert(!mkdir(sub,0700));
    path(destfile,sub,"payload");fixture(destfile);path(copied,home,"copied-folder");
    assert(!embk_file_copy(folder,copied));path(dst,copied,"sub/payload");same(dst);
    path(dst,folder,"nested-copy");assert(embk_file_copy(folder,dst)<0 && errno==EINVAL);
    path(dst,folder,"link");assert(!symlink(src,dst));
    char rejected[1024];path(rejected,home,"refused-folder");assert(embk_file_copy(folder,rejected)<0 && errno==ENOTSUP);
    assert(access(rejected,F_OK)<0);unlink(dst);
    assert(!embk_file_trash(src,home,record,sizeof record));assert(access(src,F_OK)<0);
    fixture(src);assert(embk_file_restore(record,home)<0 && errno==EEXIST);same(src);
    unlink(src);assert(!embk_file_restore(record,home));same(src);
    assert(embk_file_trash(home,home,record,sizeof record)<0);
    assert(!embk_file_trash(folder,home,record,sizeof record));assert(!embk_file_restore(record,home));same(destfile);
    unlink(src);unlink(destfile);rmdir(sub);rmdir(folder);unlink(dst);
    path(dst,copied,"sub/payload");unlink(dst);path(dst,copied,"sub");rmdir(dst);rmdir(copied);rmdir(trash);
    assert(!rmdir(home));
    puts("file operations: short writes, failed reads/flushes, same-path and overwrite refusal, tree copy, durable Trash/restore and conflicts OK");
}
