/* Native system-device ABI acceptance: bounds, capability attenuation, cached
 * status and optional AML fixture. Never changes a real saved Wi-Fi profile. */
#include <stdio.h>
#include <string.h>
#include "embk.h"
#include "oscfg.h"
#include "fileops.h"
#include "document.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)) {printf("devicecheck: line %d failed: %s\n",__LINE__,#x);return __LINE__%200+1;}} while(0)
static int restricted(int parent_menu) {
    if(parent_menu>0){CHECK(embk_win_menu((uint32_t)parent_menu,1)==-22);CHECK(embk_win_close_ack((uint32_t)parent_menu)==-22);}
    struct embk_wifi_info wifi;
    CHECK(embk_wifi_get(&wifi)==0);
    CHECK(embk_wifi_change((const void *)(uintptr_t)-1)==-1);
    CHECK(embk_audio_get((void *)(uintptr_t)-1)==-1);
    CHECK(embk_audio_volume(50,0)==-1);
    CHECK(embk_display_brightness(50)==-1);
    CHECK(embk_power_request(EMBK_POWER_OFF)==-1);
    CHECK(embk_policy_set((void *)(uintptr_t)-1)==-1);
    return 0;
}
static int preferences_child(void) {
    struct oscfg written,read;oscfg_defaults(&written);written.volume=37;written.accent=2;
    CHECK(!oscfg_save(&written));oscfg_load(&read);CHECK(!memcmp(&read,&written,sizeof read));
    written.volume=63;written.muted=1;CHECK(!oscfg_save(&written));oscfg_load(&read);
    CHECK(!memcmp(&read,&written,sizeof read));
    CHECK(embk_open("/data/settings.conf",EMBK_O_CREAT|EMBK_O_WRONLY,0600)<0);
    const char *path=oscfg_path();CHECK(!unlink(path));return 0;
}
static int preferences(const char *mode) {
    char home[96],environment[112];
    snprintf(home,sizeof home,"/home/.devicecheck-%lu-%lu",(unsigned long)embk_getpid(),(unsigned long)embk_uptime_ms());
    CHECK(!embk_mkdir(home));snprintf(environment,sizeof environment,"HOME=%s",home);
    char *args[]={"/data/apps/devicecheck/devicecheck.elf",(char *)mode,NULL};
    char *env[]={environment,NULL};struct embk_spawn_file_action actions[4];
    embk_action_set_caps(&actions[0],EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM));
    embk_action_ns_bind(&actions[1],"/system",EMBK_NS_RO);
    embk_action_ns_bind(&actions[2],"/data/apps",EMBK_NS_RO);
    embk_action_ns_bind(&actions[3],home,EMBK_NS_RW);
    int handle=embk_spawn_env(args[0],args,env,actions,4);CHECK(handle>=0);
    CHECK(embk_wait(handle)==0);CHECK(!embk_rmdir(home));return 0;
}
static int files_child(void) {
    const char *home=getenv("HOME");CHECK(home && home[0]=='/');
    char src[180],copy[180],trash[180],folder[180],nested[180],clone[180],record[256],copied[220];
    snprintf(src,sizeof src,"%s/source",home);snprintf(copy,sizeof copy,"%s/copy",home);
    snprintf(trash,sizeof trash,"%s/Trash",home);CHECK(!embk_mkdir(trash));
    unsigned char data[4096];for(unsigned i=0;i<sizeof data;i++)data[i]=(unsigned char)(i*17+9);
    int f=(int)embk_open(src,EMBK_O_CREAT|EMBK_O_EXCL|EMBK_O_WRONLY,0600);CHECK(f>=0);
    for(unsigned i=0;i<17;i++)CHECK(embk_write(f,data,sizeof data)==sizeof data);
    CHECK(!fsync(f));CHECK(!embk_close(f));
    CHECK(embk_file_copy(src,src)<0);CHECK(!embk_file_copy(src,copy));
    CHECK(embk_file_copy(src,copy)<0 && errno==EEXIST);
    f=(int)embk_open(copy,EMBK_O_RDONLY,0);CHECK(f>=0);unsigned char got[4096];
    for(unsigned i=0;i<17;i++){CHECK(embk_read(f,got,sizeof got)==sizeof got);CHECK(!memcmp(got,data,sizeof got));}
    CHECK(embk_read(f,got,1)==0);embk_close(f);
    CHECK(!embk_file_trash(src,home,record,sizeof record));CHECK(embk_open(src,EMBK_O_RDONLY,0)<0);
    CHECK(!embk_file_restore(record,home));
    snprintf(folder,sizeof folder,"%s/folder",home);snprintf(nested,sizeof nested,"%s/folder/file",home);
    snprintf(clone,sizeof clone,"%s/cloned-folder",home);CHECK(!embk_mkdir(folder));
    CHECK(!embk_file_move(src,nested));CHECK(!embk_file_copy(folder,clone));
    CHECK(!embk_file_trash(folder,home,record,sizeof record));CHECK(!embk_file_restore(record,home));
    snprintf(copied,sizeof copied,"%s/file",clone);
    CHECK(!embk_unlink(nested));CHECK(!embk_rmdir(folder));CHECK(!embk_unlink(copied));CHECK(!embk_rmdir(clone));
    CHECK(!embk_unlink(copy));CHECK(!embk_rmdir(trash));
    struct emdoc_stamp absent,original,latest;
    CHECK(!emdoc_probe(src,&absent) && !absent.exists);
    CHECK(!emdoc_save(src,"original",8,&absent,&original));
    char text[128]="preserved";
    CHECK(!emdoc_read(src,text,sizeof text,&latest) && !strcmp(text,"original"));
    CHECK(emdoc_save(src,"stale",5,&absent,0)<0 && errno==EAGAIN);
    CHECK(!emdoc_save(src,"external",8,&original,&latest));
    CHECK(emdoc_save(src,"lost",4,&original,0)<0 && errno==EAGAIN);
    CHECK(!emdoc_read(src,text,sizeof text,0) && !strcmp(text,"external"));
    CHECK(!emdoc_save(src,"binary\0tail",11,&latest,&original));
    strcpy(text,"preserved");CHECK(emdoc_read(src,text,sizeof text,0)<0 && errno==EILSEQ && !strcmp(text,"preserved"));
    CHECK(!embk_unlink(src));
    struct emdoc_draft meta={.cursor=3,.original=original},loaded;
    snprintf(meta.path,sizeof meta.path,"%s",src);
    CHECK(!emdoc_draft_save(copy,&meta,"unsaved draft"));
    CHECK(!emdoc_draft_load(copy,&loaded,text,sizeof text));
    CHECK(!strcmp(text,"unsaved draft") && !strcmp(loaded.path,src) && loaded.cursor==3);
    CHECK(!embk_unlink(copy));
    puts("devicecheck: durable replacement, external edit conflict, binary refusal and draft recovery OK");
    puts("devicecheck: confined file copy, overwrite refusal, directory copy, move, Trash and restore OK");return 0;
}
static int policy_fixture(void) {
    struct embk_display_info d;CHECK(!embk_display_get(&d));
    CHECK(d.level_count==6 && d.percent==60 && (d.flags&EMBK_DISPLAY_KNOWN));
    struct embk_policy original,p;CHECK(!embk_policy_get(&original));
    CHECK(embk_syscall2(EMBK_SYS_policy_get,(uintptr_t)&p,sizeof p-1)==-22);
    CHECK(embk_policy_get((void *)(uintptr_t)-1)==-14);
    p=original;p.flags=0;p.profile=0;p.idle_dim_seconds=10;p.idle_lock_seconds=0;
    CHECK(!embk_policy_set(&p));
    uint64_t until=embk_uptime_ms()+18000;
    do {embk_sleep_ms(100);CHECK(!embk_display_get(&d));}while(d.percent!=20 && embk_uptime_ms()<until);
    int dimmed=d.percent==20;
    original.flags&=EMBK_POLICY_LID_LOCK;CHECK(!embk_policy_set(&original));
    until=embk_uptime_ms()+5000;
    do {embk_sleep_ms(100);CHECK(!embk_display_get(&d));}while(d.percent!=60 && embk_uptime_ms()<until);
    CHECK(dimmed && d.percent==60);
    CHECK(embk_session(EMBK_SESSION_UNLOCK,0)==-1);
    puts("devicecheck: policy syscall bounds, idle ACPI dim/restore and session controller boundary OK");return 0;
}
static int session_writer(void) {
    const char *home=getenv("HOME");CHECK(home && home[0]=='/');
    char path[220];snprintf(path,sizeof path,"%s/Documents/.logout-write-check",home);
    int fd=(int)embk_open(path,EMBK_O_CREAT|EMBK_O_TRUNC|EMBK_O_WRONLY,0600);CHECK(fd>=0);
    unsigned char *data=malloc(65536);CHECK(data);memset(data,0x5a,65536);
    for(;;) {
        CHECK(embk_lseek(fd,0,EMBK_SEEK_SET)==0);
        CHECK(embk_write(fd,data,65536)==65536);
        CHECK(!fsync(fd));
    }
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--session-writer"))return session_writer();
    if(argc>1 && !strcmp(argv[1],"--abandon-audio")) {CHECK(!embk_audio_open());return 0;}
    if(argc>1 && !strcmp(argv[1],"--preferences-child")) return preferences_child();
    if(argc>1 && !strcmp(argv[1],"--preferences")) return preferences("--preferences-child");
    if(argc>1 && !strcmp(argv[1],"--files")) return preferences("--files-child");
    if(argc>1 && !strcmp(argv[1],"--files-child")) return files_child();
    if(argc>1 && !strcmp(argv[1],"--policy")) return policy_fixture();
    if(argc>1 && !strcmp(argv[1],"--restricted")) return restricted(argc>2?atoi(argv[2]):0);
    if(argc>1 && !strcmp(argv[1],"--menu-no-gpu")) {CHECK(embk_win_menu(0,1)==-1);CHECK(embk_win_close_ack(0)==-1);CHECK(embk_desktop_control(EMBK_DESKTOP_GET,(void *)(uintptr_t)-1)==-1);return 0;}
    struct embk_wifi_info wifi;struct embk_audio_info audio;struct embk_display_info display;
    CHECK(embk_wifi_get(&wifi)==0 && wifi.version==1 && wifi.saved_count<=8 && wifi.network_count<=24);
    CHECK(embk_syscall2(EMBK_SYS_wifi_info,(uintptr_t)&wifi,sizeof wifi-1)==-22);
    CHECK(embk_wifi_get((void *)(uintptr_t)-1)==-14);
    struct embk_wifi_request request={.version=2,.action=EMBK_WIFI_CONNECT};
    CHECK(embk_wifi_change(&request)==-22);
    CHECK(embk_wifi_change((void *)(uintptr_t)-1)==-14);
    CHECK(embk_audio_get(&audio)==0 && audio.version==1 && audio.percent<=100);
    CHECK(embk_audio_volume(101,0)==-22 && embk_audio_volume(50,2)==-22);
    CHECK(embk_audio_get((void *)(uintptr_t)-1)==-14);
    CHECK(embk_display_get(&display)==0 && display.version==1 && display.level_count<=32);
    CHECK(embk_display_brightness(101)==-22);
    CHECK(embk_display_get((void *)(uintptr_t)-1)==-14);
    struct embk_desktop desktop={.version=EMBK_DESKTOP_VERSION};
    CHECK(!embk_desktop_control(EMBK_DESKTOP_GET,&desktop));
    CHECK(desktop.workspace_count>=1 && desktop.workspace_count<=6);
    CHECK(embk_desktop_control(EMBK_DESKTOP_CONFIGURE,&desktop)==-1);
    CHECK(embk_desktop_control(99,&desktop)==-22);
    CHECK(embk_syscall3(EMBK_SYS_desktop_control,0,(int64_t)(uintptr_t)&desktop,sizeof desktop-1)==-22);
    CHECK(embk_desktop_control(EMBK_DESKTOP_GET,(void *)(uintptr_t)-1)==-14);
    desktop.version=0;CHECK(embk_desktop_control(EMBK_DESKTOP_GET,&desktop)==-22);
    CHECK(embk_win_menu(0,1)==-22 && embk_win_menu(0,2)==-22);
    void *menu_pixels=0;
    int menu=embk_win_create_shared_ex(200,80,10,32,"menu ownership",
                                      EMBK_WINF_TRANSLUCENT,&menu_pixels);
    CHECK(menu>0 && menu_pixels);
    CHECK(embk_win_resize(menu,200,26,&menu_pixels)>=0 && menu_pixels);
    CHECK(embk_win_resize(menu,200,80,&menu_pixels)>=0 && menu_pixels);
    CHECK(!embk_win_close_ack((uint32_t)menu));
    CHECK(embk_win_close_ack(0)==-22);
    CHECK(embk_syscall1(EMBK_SYS_win_close_ack,((uint64_t)1<<32)|(uint32_t)menu)==-22);
    CHECK(!embk_win_menu((uint32_t)menu,1));
    CHECK(embk_syscall2(EMBK_SYS_win_menu,((uint64_t)1<<32)|(uint32_t)menu,1)==-22);
    char menu_id[16];snprintf(menu_id,sizeof menu_id,"%u",(unsigned)menu);
    char *child[]={"/data/apps/devicecheck/devicecheck.elf","--restricted",menu_id,0};
    struct embk_spawn_file_action action;embk_action_set_caps(&action,EMBK_CAP_BIT(EMBK_CAP_NETWORK)|EMBK_CAP_BIT(EMBK_CAP_GPU));
    int handle=embk_spawn(child[0],child,&action,1);CHECK(handle>=0);CHECK(embk_wait(handle)==0);
    CHECK(!embk_win_menu((uint32_t)menu,0));CHECK(!embk_win_destroy(menu));
    CHECK(embk_win_menu((uint32_t)menu,1)==-22);CHECK(embk_win_close_ack((uint32_t)menu)==-22);
    child[1]="--menu-no-gpu";child[2]=NULL;
    embk_action_set_caps(&action,EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM));
    handle=embk_spawn(child[0],child,&action,1);CHECK(handle>=0);CHECK(embk_wait(handle)==0);
    if(argc>1 && !strcmp(argv[1],"--fixture")) {
        CHECK((display.flags&(EMBK_DISPLAY_BACKLIGHT|EMBK_DISPLAY_KNOWN))==(EMBK_DISPLAY_BACKLIGHT|EMBK_DISPLAY_KNOWN));
        CHECK(display.level_count==6 && display.percent==60);
        struct embk_power_info power;CHECK(!embk_power_get(&power));
        CHECK((power.flags&EMBK_POWER_LID_KNOWN) && !(power.flags&EMBK_POWER_LID_CLOSED));
        CHECK(!embk_display_brightness(37));
        for(unsigned i=0;i<100;i++) {
            embk_sleep_ms(20);CHECK(!embk_display_get(&display));
            if(!(display.flags&EMBK_DISPLAY_BUSY)) break;
        }
        CHECK(!(display.flags&EMBK_DISPLAY_BUSY) && !display.last_error && display.percent==40);
        CHECK(!embk_display_brightness(60));
    }
    if(audio.flags&EMBK_AUDIO_AVAILABLE) {
        CHECK(!embk_audio_volume(50,1));CHECK(!embk_audio_get(&audio));
        CHECK(audio.percent==50 && (audio.flags&EMBK_AUDIO_MUTED));
        CHECK(!embk_audio_volume(100,0));
    } else CHECK(embk_audio_volume(50,0)==-19);
    if(argc>1 && !strcmp(argv[1],"--audio")) {
        CHECK(audio.flags&EMBK_AUDIO_AVAILABLE);CHECK(audio.sample_rate==48000);
        CHECK(!embk_audio_volume(100,0));
        for(unsigned round=0;round<2;round++) {
            CHECK(!embk_audio_open());
            unsigned done=0,total=round?24000:144000;uint64_t deadline=embk_uptime_ms()+15000;
            int16_t frames[2048];
            while(done<total) {
                unsigned count=total-done;if(count>1024) count=1024;
                for(unsigned i=0;i<count;i++) frames[2*i]=frames[2*i+1]=((done+i)%109)<54?8000:-8000;
                int n=embk_audio_write(frames,count);CHECK(n>=0);done+=n;
                CHECK(embk_uptime_ms()<deadline);if(!n) embk_sleep_ms(1);
            }
            while(!embk_audio_drained()) {CHECK(embk_uptime_ms()<deadline);embk_sleep_ms(1);}
            CHECK(!embk_audio_close());
        }
        char *args[]={"/data/apps/devicecheck/devicecheck.elf","--abandon-audio",NULL};
        int child=embk_spawn(args[0],args,NULL,0);CHECK(child>=0);
        uint64_t deadline=embk_uptime_ms()+5000;
        while(embk_proc_alive(child)) {CHECK(embk_uptime_ms()<deadline);embk_sleep_ms(1);}
        CHECK(!embk_audio_open()); /* released at exit, even before wait */
        CHECK(!embk_wait(child));CHECK(!embk_audio_close());
    }
    puts("devicecheck: device status, bounds and capability gates OK");return 0;
}
