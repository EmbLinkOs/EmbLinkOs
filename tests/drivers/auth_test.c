#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define __EMBK_H__
static char auth_passwd[128],auth_shadow[128];
#define EMBK_AUTH_PASSWD auth_passwd
#define EMBK_AUTH_SHADOW auth_shadow
#define EMBK_O_RDONLY O_RDONLY
#define EMBK_O_WRONLY O_WRONLY
#define EMBK_O_CREAT O_CREAT
#define EMBK_O_TRUNC O_TRUNC
struct embk_stat {uint64_t size;uint32_t type;};
#define EMBK_DT_DIR 2
static int embk_mkdir(const char *p){return mkdir(p,0755); }
static int embk_open(const char *p,int f,int m){return open(p,f,m);}
static int64_t embk_read(int fd,void *p,size_t n){return read(fd,p,n);}
static int64_t embk_write(int fd,const void *p,size_t n){return write(fd,p,n);}
static int embk_close(int fd){return close(fd);}
static int embk_unlink(const char *p){return unlink(p);}
static int embk_stat(const char *p,struct embk_stat *v){struct stat s;int r=stat(p,&s);if(!r){v->size=s.st_size;v->type=S_ISDIR(s.st_mode)?2:1;}return r;}
static int sync_count,fail_sync;
static int model_sync(int fd){if(++sync_count==fail_sync){errno=EIO;return -1;}return fsync(fd);}
#define embk_fsync model_sync
#include "../../user/lib/auth.c"
#undef embk_fsync
static void write_fixture(const char *p,const char *s){FILE *f=fopen(p,"w");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
int main(void){
    char dir[]="/tmp/emblink-auth-XXXXXX";assert(mkdtemp(dir));
    snprintf(auth_passwd,sizeof auth_passwd,"%s/passwd",dir);snprintf(auth_shadow,sizeof auth_shadow,"%s/shadow",dir);
    embk_auth_set_dir(dir);embk_auth_set_iterations(1000);
    write_fixture(auth_passwd,"");write_fixture(auth_shadow,"");
    assert(!embk_auth_valid_username("../root") && !embk_auth_valid_username("Bad") && embk_auth_valid_username("laptop-user"));
    assert(!embk_auth_has_accounts());
    fail_sync=2;assert(embk_auth_create("alice","eight-characters")<0);
    assert(!embk_auth_verify("alice","eight-characters") && !embk_auth_has_accounts());
    fail_sync=0;assert(!embk_auth_create("alice","eight-characters"));assert(embk_auth_has_accounts());
    assert(embk_auth_verify("alice","eight-characters") && !embk_auth_verify("alice","wrong"));
    assert(embk_auth_create("alice","eight-characters")==EMBK_AUTH_EEXIST);
    /* An independently-derived legacy PBKDF2 vector is written by the runner. */
    uint8_t salt[16]={0},got[32];derive("password",salt,1,got);
    char hex[65];to_hex(got,32,hex);assert(!strcmp(hex,"1fefe125ab13dd2c06db86711ec448e9490e6c73024d7d8d659126c6f9fadd68"));
    write_fixture(auth_shadow,"a\nx\nalice:$pbkdf2-sha256$4294967297$00$00:\n");assert(!embk_auth_verify("alice","password"));
    write_fixture(auth_shadow,"alice:$pbkdf2-sha256$4294967297$00000000000000000000000000000000$1fefe125ab13dd2c06db86711ec448e9490e6c73024d7d8d659126c6f9fadd68:\n");
    assert(!embk_auth_verify("alice","password"));
    write_fixture(auth_shadow,"alice:\n");assert(!embk_auth_verify("alice","password"));
    write_fixture(auth_shadow,"a\nx\nz");assert(!embk_auth_verify("long_username_that_does_not_fit","password"));
    write_fixture(auth_shadow,"alice:$pbkdf2-sha256$0$00000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000:\n");assert(!embk_auth_verify("alice","password"));
    /* A passwd-only interrupted transaction must be recoverable without duplicates. */
    write_fixture(auth_shadow,"");assert(!embk_auth_create("alice","eight-characters"));
    size_t n;char *text=read_file(auth_passwd,&n);assert(text && n);
    assert(strstr(text,"alice:") && !strstr(text,"\nalice:"));free(text);
    unlink(auth_passwd);unlink(auth_shadow);assert(!rmdir(dir));
    puts("authentication: durable creation, interrupted commit retry, verification and hostile records OK");
}
