/* Malformed IPC must never turn an advertised length into an out-of-bounds
 * read or a truncated filename into a different file. No host syscalls. */
#define __EMBK_H__
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define EMBK_EINVAL 22
#define EMBK_E2BIG 7
#define EMBK_ENAMETOOLONG 36
static unsigned char response[512];
static unsigned response_bytes,closed;
static int embk_chan_listen(const char *p){(void)p;return 1;}
static int embk_chan_connect(const char *p){(void)p;return 1;}
static int embk_chan_accept(int h){return h;}
static int embk_chan_close(int h){(void)h;closed++;return 0;}
static int embk_chan_send(int h,const void *b,unsigned n,const void *a,unsigned ac,int f){(void)h;(void)b;(void)n;(void)a;(void)ac;(void)f;return 0;}
static int embk_chan_recv(int h,void *b,unsigned cap,unsigned *n,void *a,void *ac){(void)h;(void)a;(void)ac;if(response_bytes>cap)abort();memcpy(b,response,response_bytes);*n=response_bytes;return 0;}
#include "emfiles.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}count++;}while(0)
int main(void) {
    int count=0;char path[256]="unchanged";
    struct emsvc_head h={EMSVC_T_FILE_PANEL,sizeof(struct emfile_rep)};
    struct emfile_rep r={.chosen=1,.handle=-1};strcpy(r.path,"/home/tester/document.txt");
    memcpy(response,&h,sizeof h);memcpy(response+sizeof h,&r,sizeof r);response_bytes=sizeof h+sizeof r;
    CHECK(embk_file_panel(EMFILE_OPEN,0,0,0,path,sizeof path)==1 && !strcmp(path,r.path));
    strcpy(path,"unchanged");response_bytes=sizeof h;
    CHECK(embk_file_panel(EMFILE_OPEN,0,0,0,path,sizeof path)==-EMBK_EINVAL && !strcmp(path,"unchanged"));
    h.len=UINT32_MAX;memcpy(response,&h,sizeof h);
    CHECK(emsvc_call("files",EMSVC_T_FILE_PANEL,0,0,path,sizeof path)==-EMBK_EINVAL);
    uint32_t type;unsigned len;CHECK(emsvc_accept(1,&type,path,sizeof path,&len)==-EMBK_EINVAL);
    h.len=sizeof r;memcpy(response,&h,sizeof h);response_bytes=sizeof h+sizeof r;
    memset(r.path,'x',sizeof r.path);memcpy(response+sizeof h,&r,sizeof r);
    CHECK(embk_file_panel(EMFILE_SAVE,0,0,0,path,sizeof path)==-EMBK_EINVAL);
    strcpy(r.path,"relative");memcpy(response+sizeof h,&r,sizeof r);
    CHECK(embk_file_panel(EMFILE_OPEN,0,0,0,path,sizeof path)==-EMBK_EINVAL);
    strcpy(r.path,"/home/tester/document.txt");memcpy(response+sizeof h,&r,sizeof r);
    CHECK(embk_file_panel(EMFILE_SAVE,0,0,0,path,4)==-EMBK_ENAMETOOLONG && !strcmp(path,"unchanged"));
    CHECK(embk_file_panel(2,0,0,0,path,sizeof path)==-EMBK_EINVAL);
    CHECK(closed==7);CHECK(emsvc_listen("files")==1);CHECK(!emsvc_reply(1,1,0,0));
    printf("service-test: %d assertions passed\n",count);return 0;
}
