/* Real service + station + crypto, model radio and transactional VFS. No RF
 * execution is implied. Exercises persistence refusal and public snapshots. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/wifi/wifi.c"
static uint8_t disk[WIFI_STORE_BYTES],temp[WIFI_STORE_BYTES];
static unsigned disk_size,temp_size,exists,dir_exists,temp_exists,depth,syncs,fail_sync;
static struct ax200_status radio={.state=AX200_RADIO_READY,.mac={2,3,4,5,6,7}};
static struct vfs_mount mount;
void kprintf(const char *f,...) {(void)f;}
void net_lock(void) {assert(!depth);depth=1;}
void net_unlock(void) {assert(depth==1);depth=0;}
void net_wait(void) {assert(0);}
void net_rx(const uint8_t *p,uint32_t n) {(void)p;(void)n;}
void arp_reset(void) {}
bool net_dhcp(void) {return 0;}
void net_dhcp_forget_lease(void) {}
uint64_t net_dhcp_renew_after_ms(void) {return UINT64_MAX;}
uint64_t net_dhcp_lease_remaining_ms(void) {return 0;}
uint64_t time_get_us(void) {return 1000000;}
struct netif g_netif;
int crypto_hw_random(uint8_t *p,unsigned n) {memset(p,1,n);return 0;}
struct thread *process_create_kthread(void (*fn)(void),struct process *parent) {(void)fn;(void)parent;return (struct thread *)(uintptr_t)1;}
void ax200_probe(void) {}
const struct ax200_status *ax200_get_status(void) {return &radio;}
int ax200_scan_channel(uint8_t c) {assert(c==1);return 0;}
int ax200_prepare_bss(const struct wifi_bss *b) {(void)b;return 0;}
int ax200_associate_bss(const struct wifi_bss *b) {(void)b;return 0;}
int ax200_tx(const uint8_t *p,unsigned n) {(void)p;(void)n;return 0;}
void ax200_poll(void) {}
int ax200_receive(uint8_t *p,unsigned n,uint8_t *c) {(void)p;(void)n;(void)c;return 0;}
int ax200_channels(uint8_t *p,unsigned n) {assert(n);p[0]=1;return 1;}
void ax200_leave(void) {}
static int lookup(struct vnode *dir,const char *name,size_t n,struct vnode *out) {
    assert(!depth && dir->ino==2);
    if(n==12 && !memcmp(name,"wifi.pending",n) && temp_exists) {*out=(struct vnode){.mnt=&mount,.ino=3};return 0;}
    return -2;
}
static int mkdir_model(struct vnode *dir,const char *name,size_t n,struct vnode *out) {
    assert(!depth && dir->ino==1 && n==6 && !memcmp(name,"system",n));dir_exists=1;
    *out=(struct vnode){.mnt=&mount,.ino=2};return 0;
}
static int create(struct vnode *dir,const char *name,size_t n,uint32_t mode,struct vnode *out) {
    assert(!depth && dir->ino==2 && n==12 && !memcmp(name,"wifi.pending",n) && mode==0600);
    temp_exists=1;temp_size=0;*out=(struct vnode){.mnt=&mount,.ino=3};return 0;
}
static int unlink_model(struct vnode *dir,const char *name,size_t n) {
    assert(!depth && dir->ino==2 && n==12 && !memcmp(name,"wifi.pending",n));temp_exists=0;return 0;
}
static int write_model(struct vnode *file,uint64_t off,const void *p,size_t n,size_t *written) {
    assert(!depth && file->ino==3 && !off && n<=sizeof temp);memcpy(temp,p,n);temp_size=n;*written=n;return 0;
}
static int sync_model(struct vnode *node) {
    assert(!depth && (node->ino==2 || node->ino==3));syncs++;return fail_sync==syncs?-5:0;
}
static int rename_model(struct vnode *old,const char *a,size_t an,struct vnode *new,const char *b,size_t bn) {
    assert(!depth && old->ino==2 && new->ino==2 && an==12 && bn==13 && !memcmp(a,"wifi.pending",an) && !memcmp(b,"wifi.profiles",bn));
    assert(temp_exists);memcpy(disk,temp,temp_size);disk_size=temp_size;exists=1;temp_exists=0;return 0;
}
static const struct vfs_ops ops={.lookup=lookup,.mkdir=mkdir_model,.create=create,.unlink=unlink_model,.write=write_model,.sync=sync_model,.rename=rename_model};
int vfs_resolve(const char *path,struct vnode *out) {
    assert(!depth);
    if(!strcmp(path,"/data")) {*out=(struct vnode){.mnt=&mount,.ino=1};return 0;}
    if(!strcmp(path,STORE_DIR) && dir_exists) {*out=(struct vnode){.mnt=&mount,.ino=2};return 0;}
    return -2;
}
int vfs_stat(const char *path,struct vfs_stat *out) {assert(!depth);if(strcmp(path,STORE_PATH)||!exists) return -2;out->size=disk_size;return 0;}
int vfs_read(const char *path,uint64_t off,void *p,size_t n,size_t *read) {
    assert(!depth && !strcmp(path,STORE_PATH) && !off && n==disk_size);memcpy(p,disk,n);*read=n;return 0;
}
static void complete(struct embk_wifi_request *r,int error) {
    int id=wifi_request(r);assert(id>0);assert(wifi_request(r)==-16);
    net_lock();process_pending();net_unlock();
    struct embk_wifi_info info;assert(!wifi_snapshot(&info));assert(info.completed_id==(unsigned)id && info.request_error==error && !(info.flags&EMBK_WIFI_BUSY));
    for(unsigned i=0;i<sizeof pending;i++) assert(!((uint8_t *)&pending)[i]);
}
int main(void) {
    mount.ops=&ops;uint8_t mac[6];assert(wifi_init(mac));wifi_start_worker();
    struct embk_wifi_request r={.version=1,.action=EMBK_WIFI_CONNECT,.ssid_len=4,.secret_len=8};
    memcpy(r.ssid,"IEEE",4);memcpy(r.secret,"password",8);complete(&r,0);
    assert(saved.count==1 && have_profile && want_join && exists && !temp_exists);
    struct wifi_saved_profiles loaded={0};load_saved(&loaded);assert(!memcmp(&loaded,&saved,sizeof saved));
    uint8_t previous[WIFI_STORE_BYTES];memcpy(previous,disk,disk_size);unsigned previous_size=disk_size;
    r.ssid[0]='N';fail_sync=syncs+1;complete(&r,-5);
    assert(saved.count==1 && profile.ssid[0]=='I' && disk_size==previous_size && !memcmp(previous,disk,disk_size) && !temp_exists);
    fail_sync=0;complete(&r,0);assert(saved.count==2 && profile.ssid[0]=='N');
    struct embk_wifi_info info;wifi_snapshot(&info);assert(info.saved_count==2 && info.saved[0].ssid[0]=='N');
    memset(&r,0,sizeof r);r.version=1;r.action=EMBK_WIFI_RECONNECT;r.ssid_len=4;memcpy(r.ssid,"IEEE",4);complete(&r,0);
    assert(profile.ssid[0]=='I' && saved.items[0].ssid[0]=='I');
    r.action=EMBK_WIFI_FORGET;complete(&r,0);assert(!want_join && !have_profile && saved.count==1);
    memcpy(r.ssid,"NEEE",4);complete(&r,0);assert(!saved.count);
    memset(&loaded,0xff,sizeof loaded);load_saved(&loaded);assert(!loaded.count); /* empty store suppresses legacy import */
    r.action=EMBK_WIFI_SCAN;r.ssid_len=0;complete(&r,0);assert(scanning);
    r.action=EMBK_WIFI_DISCONNECT;complete(&r,0);assert(!scanning && !want_join);
    puts("Wi-Fi service: asynchronous requests, secret wiping, atomic profiles, disk errors and forget/reload OK");
}
