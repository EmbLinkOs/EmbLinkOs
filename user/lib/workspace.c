#include "workspace.h"
#include "embk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
static void filename(struct emdoc_workspace *w,int slot,char *out,size_t cap) {
    snprintf(out,cap,"%s/%s-%d.draft",w->directory,w->prefix,slot);
}
int emws_init(struct emdoc_workspace *w,const char *app) {
    memset(w,0,sizeof *w);const char *home=getenv("HOME");
    if(!home || home[0]!='/'){errno=EINVAL;return -1;}
    int n=snprintf(w->directory,sizeof w->directory,"%s/.%s-drafts",home,app);
    if(n<0 || n>=(int)sizeof w->directory){errno=ENAMETOOLONG;return -1;}
    if(mkdir(w->directory,0700) && errno!=EEXIST)return -1;
    uint64_t nonce;
    if(embk_getrandom(&nonce,sizeof nonce)!=sizeof nonce){errno=EIO;return -1;}
    snprintf(w->prefix,sizeof w->prefix,"%lu-%016llx",(unsigned long)getpid(),(unsigned long long)nonce);
    w->ready=1;return 0;
}
int emws_forget(struct emdoc_workspace *w,int slot) {
    if(!w->ready || slot<0 || slot>=EMWS_SLOTS){errno=EINVAL;return -1;}
    char path[400];filename(w,slot,path,sizeof path);
    if(unlink(path)) {if(errno!=ENOENT)return -1;}
    else {
        int fd=open(w->directory,O_RDONLY);if(fd<0)return -1;
        int rc=fsync(fd),saved=errno;close(fd);errno=saved;if(rc)return -1;
    }
    w->present[slot]=0;w->hashes[slot]=0;return 0;
}
int emws_checkpoint(struct emdoc_workspace *w,int slot,const struct emdoc_draft *meta,const char *text,int dirty) {
    if(!w->ready || slot<0 || slot>=EMWS_SLOTS){errno=EINVAL;return -1;}
    if(!dirty)return w->present[slot]?emws_forget(w,slot):0;
    uint64_t h=emdoc_hash(text,strlen(text)) ^ emdoc_hash(meta->path,strlen(meta->path)) ^ meta->original.hash;
    if(w->present[slot] && w->hashes[slot]==h)return 0;
    char path[400];filename(w,slot,path,sizeof path);
    if(emdoc_draft_save(path,meta,text))return -1;
    w->hashes[slot]=h;w->present[slot]=1;return 0;
}
int emws_restore(struct emdoc_workspace *w,const char *app,size_t capacity,
                 int (*restore)(const struct emdoc_draft *,const char *)) {
    if(!w->ready)return -1;
    DIR *dir=opendir(w->directory);if(!dir)return -1;
    char *text=malloc(capacity);if(!text){closedir(dir);return -1;}
    struct embk_proc_info processes[256];int count=embk_proc_list(processes,256);
    if(count<0){free(text);closedir(dir);return -1;} /* fail closed: don't steal live drafts */
    /* Snapshot names before renaming: EMBKFS directory cursors are positional,
     * so deleting an entry while enumerating could otherwise skip its neighbor.
     * Excess abandoned drafts remain available for a later launch. */
    char (*names)[256]=calloc(128,256);
    if(!names){free(text);closedir(dir);return -1;}
    struct dirent *e;unsigned files=0;
    while((e=readdir(dir)) && files<128) {
        size_t n=strlen(e->d_name);
        if(n<7 || n>=256 || strcmp(e->d_name+n-6,".draft"))continue;
        memcpy(names[files++],e->d_name,n+1);
    }
    closedir(dir);
    int recovered=0;unsigned sequence=0;
    for(unsigned f=0;f<files;f++) {
        const char *name=names[f];
        if(!strncmp(name,w->prefix,strlen(w->prefix)) && name[strlen(w->prefix)]=='-')continue;
        unsigned long owner=strtoul(name,0,10);int live=0;
        for(int i=0;i<count;i++)if(processes[i].pid==owner && owner!=(unsigned long)getpid() &&
            processes[i].state>0 && processes[i].state<4 && strstr(processes[i].name,app)){live=1;break;}
        if(live)continue;
        char source[512],claim[512];snprintf(source,sizeof source,"%s/%s",w->directory,name);
        snprintf(claim,sizeof claim,"%s/%s-claim-%u.draft",w->directory,w->prefix,++sequence);
        /* Atomic claim: two recovering processes cannot take the same draft. */
        if(embk_syscall2(EMBK_SYS_rename_noreplace,(uintptr_t)source,(uintptr_t)claim)<0)continue;
        struct emdoc_draft meta;int slot=-1;
        if(!emdoc_draft_load(claim,&meta,text,capacity))slot=restore(&meta,text);
        if(slot>=0 && !emws_checkpoint(w,slot,&meta,text,1)){unlink(claim);recovered++;}
        else {
            /* Keep invalid drafts and unclaimed excess tabs for another try. */
            embk_syscall2(EMBK_SYS_rename_noreplace,(uintptr_t)claim,(uintptr_t)source);
        }
    }
    free(names);free(text);return recovered;
}
