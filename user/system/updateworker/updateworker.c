/* Root-owned update service. Only this PID can stage a system slot. Release
 * authenticity is independent of transport; no development key is trusted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "embk.h"
#include "embk_socket.h"
#include "tls.h"
#include "ecdsa.h"
#include "crypto/sha256.h"
#include "release.h"

static struct embk_update_info report;
static char source[256];static uint8_t key[64],catalog[RELEASE_BYTES];
static void publish(unsigned phase,const char *text,int error,int busy) {
    report.size=sizeof report;report.phase=phase;report.error=error;report.job=busy;
    snprintf(report.message,sizeof report.message,"%s",text);
    embk_update(EMBK_UPDATE_REPORT,&report,sizeof report);
    printf("updateworker: %s (%d)\n",text,error);
}
static int read_exact(const char *path,void *buf,size_t n) {
    FILE *f=fopen(path,"rb");if(!f)return -1;
    int ok=fread(buf,1,n,f)==n && fgetc(f)==EOF;fclose(f);return ok?0:-1;
}
static int config(void) {
    if(read_exact("/system/update/key",key,64))return -1;
    FILE *f=fopen("/system/update/source","rb");if(!f)return -1;
    size_t n=fread(source,1,sizeof source,f);int extra=fgetc(f);fclose(f);
    if(!n || n>=sizeof source || extra!=EOF)return -1;
    if(source[n-1]=='\n')n--;source[n]=0;
    if(!n || source[n-1]=='/')return -1;
    if(strncmp(source,"https://",8) && strncmp(source,"http://10.0.2.2:",16) && strncmp(source,"http://127.0.0.1:",17))return -1;
    for(size_t i=0;i<n;i++)if((unsigned char)source[i]<=32 || (unsigned char)source[i]>=127 || source[i]=='#' || source[i]=='?' || source[i]=='@')return -1;
    return 0;
}
struct transport {int fd;struct tls_conn *tls;};
static long receive(struct transport *t,void *p,size_t n) {return t->tls?tls_read(t->tls,p,n):recv(t->fd,p,n,0);}
static void disconnect(struct transport *t) {if(t->tls){tls_close(t->tls);free(t->tls);}else if(t->fd>=0)close(t->fd);}
/* Deliberately bounded protocol: 200 + one exact Content-Length; no redirects,
 * compression or transfer codings that can conceal a truncated signed asset. */
static int fetch(const char *asset,uint64_t size,int (*consume)(const uint8_t *,size_t,void *),void *context) {
    int secure=!strncmp(source,"https://",8);const char *p=source+(secure?8:7),*begin=p;
    char host[128],path[384];size_t n=0;while(*p && *p!='/' && *p!=':' && n<sizeof host-1)host[n++]=*p++;
    host[n]=0;if(!n || (*p && *p!='/' && *p!=':'))return -1;
    int port=secure?443:80;
    if(*p==':'){port=0;p++;unsigned digits=0;while(*p>='0' && *p<='9'){if(++digits>5)return -1;port=port*10+*p++-'0';}if(!digits || port<1 || port>65535)return -1;}
    if(*p && *p!='/')return -1;
    if(snprintf(path,sizeof path,"%s/%s",p,asset)>=(int)sizeof path)return -1;
    struct in_addr address;if(emb_resolve(host,&address))return -1;
    struct transport t={socket(AF_INET,SOCK_STREAM,0),NULL};if(t.fd<0)return -1;
    struct sockaddr_in sa={0};sa.sin_family=AF_INET;sa.sin_port=htons(port);sa.sin_addr=address;
    if(connect(t.fd,(struct sockaddr *)&sa,sizeof sa)){disconnect(&t);return -1;}
    if(secure){t.tls=malloc(sizeof *t.tls);if(!t.tls){disconnect(&t);return -1;}if(tls_connect(t.tls,t.fd,host)){disconnect(&t);return -1;}}
    char request[640];int r=snprintf(request,sizeof request,"GET %s HTTP/1.0\r\nHost: %.*s\r\nConnection: close\r\nAccept-Encoding: identity\r\nUser-Agent: EmbLinkOS-Update/1\r\n\r\n",path,(int)(p-begin),begin);
    if(r<0 || r>=(int)sizeof request){disconnect(&t);return -1;}
    for(int sent=0;sent<r;) {long got=t.tls?tls_write(t.tls,request+sent,r-sent):send(t.fd,request+sent,r-sent,0);if(got<=0){disconnect(&t);return -1;}sent+=(int)got;}
    char header[4096];size_t hn=0;int rc=-1;uint64_t deadline=embk_uptime_ms()+30000;
    while(hn<sizeof header-1) {
        long got=receive(&t,header+hn,1);
        if(got<0 && errno==EAGAIN && embk_uptime_ms()<deadline)continue;
        if(got!=1)goto out;hn++;
        if(hn>=4 && !memcmp(header+hn-4,"\r\n\r\n",4))break;
    }
    if(hn>=sizeof header-1)goto out;header[hn]=0;
    if(strncmp(header,"HTTP/1.0 200 ",13) && strncmp(header,"HTTP/1.1 200 ",13))goto out;
    unsigned lengths=0;uint64_t length=0;char *line=strstr(header,"\r\n")+2;
    while(*line && strncmp(line,"\r\n",2)) {
        char *end=strstr(line,"\r\n"),*colon=strchr(line,':');if(!end || !colon || colon>=end)goto out;
        for(char *c=line;c<colon;c++)if(*c>='A' && *c<='Z')*c+=32;
        char *value=colon+1;while(value<end && (*value==' ' || *value=='\t'))value++;
        if(colon-line==14 && !memcmp(line,"content-length",14)) {
            if(++lengths!=1 || value==end)goto out;
            for(;value<end;value++){if(*value<'0' || *value>'9' || length>(UINT64_MAX-9)/10)goto out;length=length*10+*value-'0';}
        } else if((colon-line==17 && !memcmp(line,"transfer-encoding",17)) || (colon-line==16 && !memcmp(line,"content-encoding",16)))goto out;
        line=end+2;
    }
    if(lengths!=1 || length!=size)goto out;
    uint8_t *buffer=malloc(EMBK_UPDATE_CHUNK);if(!buffer)goto out;
    uint64_t done=0;
    while(done<size) {
        size_t need=size-done;if(need>EMBK_UPDATE_CHUNK)need=EMBK_UPDATE_CHUNK;
        size_t have=0;deadline=embk_uptime_ms()+30000;
        while(have<need) {
            long got=receive(&t,buffer+have,need-have);
            if(got<0 && errno==EAGAIN && embk_uptime_ms()<deadline)continue;
            if(got<=0){free(buffer);goto out;}
            have+=(size_t)got;deadline=embk_uptime_ms()+30000;
        }
        if(consume(buffer,have,context)){free(buffer);goto out;}done+=have;
    }
    free(buffer);rc=0;
out:disconnect(&t);return rc;
}
struct memory {uint8_t *p;size_t n;};
static int memory_sink(const uint8_t *p,size_t n,void *ctx) {struct memory *m=ctx;memcpy(m->p+m->n,p,n);m->n+=n;return 0;}
struct image_sink {unsigned op;struct sha256_ctx hash;};
static int image_sink(const uint8_t *p,size_t n,void *ctx) {
    struct image_sink *sink=ctx;sha256_update(&sink->hash,p,n);return embk_update(sink->op,(void *)p,n);
}
static int download(const char *name,unsigned op,uint64_t n,const uint8_t expected[32]) {
    struct image_sink sink={.op=op};sha256_init(&sink.hash);
    int rc=fetch(name,n,image_sink,&sink);uint8_t got[32];sha256_final(&sink.hash,got);
    return rc || memcmp(expected,got,32)?-1:0;
}
static int check(struct embk_update_image *image) {
    struct memory memory={catalog,0};if(fetch("release.emu",RELEASE_BYTES,memory_sink,&memory))return -1;
    if(update_release_parse(catalog,sizeof catalog,image,report.notes))return -2;
    uint8_t hash[32];sha256(catalog,RELEASE_SIGNED_BYTES,hash);
    if(!ecdsa_verify(ec_p256(),key,key+32,hash,32,catalog+384,32,catalog+416,32))return -2;
    struct embk_update_info status;if(embk_update(EMBK_UPDATE_STATUS,&status,sizeof status))return -1;
    if(image->release<=status.release){publish(EMBK_UPDATE_IDLE,"This system is up to date on the configured channel.",0,0);return 1;}
    report.candidate_release=image->release;memcpy(report.candidate,image->version,32);return 0;
}
static int commit_download(void) {
    int rc;
    do {rc=embk_update(EMBK_UPDATE_COMMIT,NULL,0);if(rc>0)embk_yield();}while(rc>0);
    return rc;
}
static char apps_source[256];
static int read_apps_source(void) {
    FILE *f=fopen("/system/update/apps-source","rb");if(!f)return -1;
    size_t n=fread(apps_source,1,sizeof apps_source,f);int extra=fgetc(f);fclose(f);
    if(!n || n>=sizeof apps_source || extra!=EOF)return -1;
    if(apps_source[n-1]=='\n')n--;apps_source[n]=0;
    if(strncmp(apps_source,"https://",8))return -1;
    for(size_t i=0;i<n;i++)if((unsigned char)apps_source[i]<=32 || (unsigned char)apps_source[i]>=127)return -1;
    return 0;
}
static int package_command(char *command,char *arg) {
    char *argv[]={"/system/bin/pkg.elf",command,arg,NULL};
    struct embk_spawn_file_action caps;
    embk_action_set_caps(&caps,EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)|EMBK_CAP_BIT(EMBK_CAP_NETWORK));
    int h=(int)embk_spawn(argv[0],argv,&caps,1);return h<0?h:(int)embk_wait(h);
}
int main(void) {
    if(embk_update(EMBK_UPDATE_WORKER,NULL,0))return 1;
    int configured=config()==0;
    int apps_configured=configured && read_apps_source()==0;
    if(apps_configured)report.flags|=EMBK_UPDATE_APPS_CHANNEL;
    publish(EMBK_UPDATE_IDLE,configured?"Ready to check for system and application updates.":"No trusted update channel is configured for this build.",0,0);
    for(;;) {
        int job=embk_update(EMBK_UPDATE_JOB,NULL,0);
        if(job==EMBK_UPDATE_APPLICATIONS) {
            if(!apps_configured)publish(EMBK_UPDATE_ERROR,"No trusted application registry is configured for this build.",-1,0);
            else {
                publish(EMBK_UPDATE_DOWNLOADING,"Refreshing signed application packages.",0,job);
                int rc=package_command("sync",apps_source);
                if(!rc)rc=package_command("update-all",NULL);
                publish(rc?EMBK_UPDATE_ERROR:EMBK_UPDATE_IDLE,rc?"Some application updates require attention. Current published versions remain available.":"Application updates completed. Restart updated applications to use their new versions.",rc,0);
            }
        }
        if(job==EMBK_UPDATE_CHECK || job==EMBK_UPDATE_INSTALL) {
            if(!configured){publish(EMBK_UPDATE_ERROR,"No trusted update channel is configured. A release publisher must configure the source and signing key.",-1,0);continue;}
            struct embk_update_image image;
            publish(EMBK_UPDATE_CHECKING,"Checking the signed release catalog.",0,job);
            int rc=check(&image);
            if(rc>0)continue;
            if(rc<0){publish(EMBK_UPDATE_ERROR,rc==-2?"Release signature or format was rejected. Nothing was installed.":"Could not contact the update channel. Check your connection and try again.",rc,0);continue;}
            if(job==EMBK_UPDATE_CHECK){publish(EMBK_UPDATE_AVAILABLE,"A verified system release is available.",0,0);continue;}
            rc=embk_update(EMBK_UPDATE_BEGIN,&image,sizeof image);
            if(rc){publish(EMBK_UPDATE_ERROR,"This release cannot be staged on this installation.",rc,0);continue;}
            if(download("os.img",EMBK_UPDATE_WRITE_ROOT,image.root_bytes,image.root_sha256) ||
               download("boot.efi",EMBK_UPDATE_WRITE_EFI,image.efi_bytes,image.efi_sha256) || commit_download()) {
                embk_update(EMBK_UPDATE_ABORT,NULL,0);publish(EMBK_UPDATE_ERROR,"Download or verification failed. The current system remains selected.",-1,0);continue;
            }
            publish(EMBK_UPDATE_RESTART,"Verified system update is ready. Restart when convenient.",0,0);
        }
        embk_sleep_ms(250);
    }
}
