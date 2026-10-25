/* Wi-Fi owns the selected interface; UI requests are serialized by its worker.
 * Disk I/O and PBKDF2 run outside NET so receive/DHCP work keeps progressing. */
#include "wifi.h"
#include "ax200.h"
#include "station.h"
#include "control.h"
#include "wpa2_crypto.h"
#include "crypto/hw_random.h"
#include "net/net.h"
#include "drivers/timer/timer.h"
#include "process/process.h"
#include "fs/vfs.h"
#include "include/errno.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#define LEGACY_PROFILE "/system/config/wifi.conf"
#define STORE_DIR "/data/system"
#define STORE_PATH STORE_DIR "/wifi.profiles"
static struct wifi_station station;
static struct wifi_profile profile;
static struct wifi_saved_profiles saved;
static struct embk_wifi_request pending;
static struct embk_wifi_network networks[EMBK_WIFI_NETWORKS];
static unsigned network_count,request_id,completed_id,busy;
static int worker_ready,request_error,active,want_join,have_profile,need_dhcp,cleaned;
static uint64_t retry_at,dhcp_retry_at,scanned_ms,scan_deadline;
static uint8_t scan_channels[WIFI_CHANNELS_MAX];
static unsigned scanning,scan_count,scan_index;
static void erase(void *p,unsigned n) {volatile uint8_t *v=p;while(n--) *v++=0;}
static int tune(void *p,uint8_t c) {(void)p;return ax200_scan_channel(c);}
static int prepare(void *p,const struct wifi_bss *b) {(void)p;return ax200_prepare_bss(b);}
static int associate(void *p,const struct wifi_bss *b) {(void)p;return ax200_associate_bss(b);}
static int transmit(void *p,const uint8_t *b,unsigned n) {(void)p;return ax200_tx(b,n);}
static int random_bytes(void *p,uint8_t *b,unsigned n) {(void)p;return crypto_hw_random(b,n);}
static void ethernet(void *p,const uint8_t *b,unsigned n) {(void)p;net_rx(b,n);}
static void clear_address(void) {
    g_netif.dhcp=false;g_netif.ip=0;g_netif.netmask=0;g_netif.gateway=0;g_netif.dns=0;
    arp_reset();net_dhcp_forget_lease();
}
static void link(void *p,int up) {
    (void)p;int was=g_netif.up;g_netif.up=up!=0;
    if(!up) {clear_address();need_dhcp=0;}
    else if(!was) {need_dhcp=1;dhcp_retry_at=0;kprintf("wifi: WPA2 authorized, requesting DHCP address\n");}
}
static const struct wifi_radio_ops radio_ops={tune,prepare,associate,transmit,random_bytes,ethernet,link};
int wifi_init(uint8_t mac[6]) {
    ax200_probe();const struct ax200_status *s=ax200_get_status();
    if(s->state!=AX200_RADIO_READY || wifi_station_init(&station,&radio_ops,0,s->mac)) return 0;
    memcpy(mac,s->mac,6);active=1;want_join=1;return 1;
}
static void discover(const uint8_t *p,unsigned n,uint8_t ch) {
    struct embk_wifi_network entry;
    if(wifi_discovery_parse(&entry,p,n,ch)) return;
    unsigned i;
    for(i=0;i<network_count;i++) if(networks[i].ssid_len==entry.ssid_len &&
        !memcmp(networks[i].ssid,entry.ssid,entry.ssid_len) && networks[i].security==entry.security) break;
    if(i==EMBK_WIFI_NETWORKS) return;
    networks[i]=entry;if(i==network_count) network_count++;
}
static int start_scan(void) {
    if(station.state!=WIFI_STA_DOWN && station.state!=WIFI_STA_FAILED) return -EMBK_EBUSY;
    scan_count=ax200_channels(scan_channels,sizeof scan_channels);
    if(!scan_count) return -EMBK_ENODEV;
    ax200_leave();cleaned=1;network_count=0;scan_index=0;
    if(ax200_scan_channel(scan_channels[0])) return -EMBK_EIO;
    scanning=1;scan_deadline=time_get_us()/1000+250;return 0;
}
void wifi_poll(void) {
    if(!active) return;
    ax200_poll();const struct ax200_status *s=ax200_get_status();
    if(s->state!=AX200_RADIO_READY || s->rfkill) {
        scanning=0;
        if(station.state!=WIFI_STA_DOWN) wifi_station_disconnect(&station);
        if(!cleaned && s->state==AX200_RADIO_READY) ax200_leave();
        cleaned=1;retry_at=time_get_us()/1000+5000;return;
    }
    uint8_t frame[WIFI_MPDU_MAX],channel;int n;unsigned budget=32;
    uint64_t now=time_get_us()/1000;
    while(budget-- && (n=ax200_receive(frame,sizeof frame,&channel))>0) {
        if(scanning || station.state==WIFI_STA_SCAN) discover(frame,n,channel);
        if(!scanning) wifi_station_rx(&station,frame,n,channel,now);
    }
    if(scanning) {
        if(now>=scan_deadline) {
            if(++scan_index==scan_count) {scanning=0;scanned_ms=now;}
            else if(ax200_scan_channel(scan_channels[scan_index])) {scanning=0;request_error=-EMBK_EIO;}
            else scan_deadline=now+250;
        }
        return;
    }
    wifi_station_tick(&station,now);
    if(station.state==WIFI_STA_FAILED && !cleaned) {
        kprintf("wifi: connection failed (stage %u); retrying in 5s\n",station.error);
        ax200_leave();cleaned=1;retry_at=now+5000;
    }
}
int wifi_tx(const void *frame,uint32_t n) {return active?wifi_station_tx(&station,frame,n):-1;}
static int read_file(const char *path,uint8_t *buf,unsigned cap,unsigned *n) {
    struct vfs_stat st;size_t got=0;
    int rc=vfs_stat(path,&st);if(rc) return rc;
    if(st.size>cap) return -EMBK_EINVAL;
    rc=vfs_read(path,0,buf,st.size,&got);
    if(rc || got!=st.size) return rc?rc:-EMBK_EIO;
    *n=got;return 0;
}
static void load_saved(struct wifi_saved_profiles *out) {
    uint8_t buf[WIFI_STORE_BYTES];unsigned n;
    int rc=read_file(STORE_PATH,buf,sizeof buf,&n);
    if(!rc) {
        if(wifi_store_decode(out,buf,n)) kprintf("wifi: saved profiles are invalid; automatic connection disabled\n");
    } else if(rc==-EMBK_ENOENT) {
        if(!read_file(LEGACY_PROFILE,buf,sizeof buf,&n) && !wifi_profile_parse(&out->items[0],buf,n)) out->count=1;
    } else kprintf("wifi: saved profiles could not be read (%d)\n",rc);
    erase(buf,sizeof buf);
}
/* Called only in the worker's global namespace. Temp data, replacement and
 * deletion commit through EMBKFS transactions; sync reports device failures. */
static int persist(const struct wifi_saved_profiles *next) {
    struct vnode parent,dir,file;int rc=vfs_resolve("/data",&parent);
    if(rc) return rc;
    rc=vfs_resolve(STORE_DIR,&dir);
    if(rc==-EMBK_ENOENT && parent.mnt->ops->mkdir) rc=parent.mnt->ops->mkdir(&parent,"system",6,&dir);
    if(rc) return rc;
    const struct vfs_ops *ops=dir.mnt->ops;
    if(!ops->create || !ops->lookup || !ops->write || !ops->rename || !ops->unlink || !ops->sync) return -EMBK_ENOTSUP;
    rc=ops->lookup(&dir,"wifi.pending",12,&file);
    if(!rc) {rc=ops->unlink(&dir,"wifi.pending",12);if(rc) return rc;}
    else if(rc!=-EMBK_ENOENT) return rc;
    rc=ops->create(&dir,"wifi.pending",12,0600,&file);if(rc) return rc;
    uint8_t buf[WIFI_STORE_BYTES];unsigned n=wifi_store_encode(next,buf);size_t written=0;
    rc=n?ops->write(&file,0,buf,n,&written):-EMBK_EINVAL;erase(buf,sizeof buf);
    if(!rc && written!=n) rc=-EMBK_EIO;
    if(!rc) rc=ops->sync(&file);
    if(!rc) rc=ops->rename(&dir,"wifi.pending",12,&dir,"wifi.profiles",13);
    if(!rc) rc=ops->sync(&dir);
    if(rc) ops->unlink(&dir,"wifi.pending",12);
    return rc;
}
static int find_saved(const uint8_t *ssid,unsigned n) {
    for(unsigned i=0;i<saved.count;i++) if(saved.items[i].ssid_len==n && !memcmp(saved.items[i].ssid,ssid,n)) return i;
    return -1;
}
static void disconnect(void) {
    scanning=0;wifi_station_disconnect(&station);ax200_leave();cleaned=1;retry_at=0;
}
static int apply_request(const struct embk_wifi_request *r) {
    if(r->action==EMBK_WIFI_SCAN) return start_scan();
    if(r->action==EMBK_WIFI_DISCONNECT) {want_join=0;disconnect();return 0;}
    struct wifi_saved_profiles next=saved;struct wifi_profile fresh={0};
    int index=find_saved(r->ssid,r->ssid_len),rc=0;
    if(r->action==EMBK_WIFI_FORGET) {
        if(index<0) rc=-EMBK_ENOENT;
        else {
            for(unsigned i=index;i+1<next.count;i++) next.items[i]=next.items[i+1];
            erase(&next.items[--next.count],sizeof fresh);
        }
    } else if(r->action==EMBK_WIFI_RECONNECT) {
        if(!r->ssid_len) index=saved.count?0:-1;
        if(index<0) rc=-EMBK_ENOENT;
        else fresh=saved.items[index];
    } else {
        fresh.ssid_len=r->ssid_len;memcpy(fresh.ssid,r->ssid,r->ssid_len);
        net_unlock();rc=wifi_wpa2_psk(r->secret,r->secret_len,r->ssid,r->ssid_len,fresh.pmk);net_lock();
        if(rc) rc=-EMBK_EINVAL;
        if(index<0 && next.count==EMBK_WIFI_SAVED) rc=-EMBK_ENOSPC;
    }
    if(!rc && r->action!=EMBK_WIFI_FORGET) {
        if(index<0) index=next.count++;
        /* Most recently selected network is the preferred boot profile. */
        for(int i=index;i>0;i--) next.items[i]=next.items[i-1];
        next.items[0]=fresh;
    }
    if(!rc) {net_unlock();rc=persist(&next);net_lock();}
    if(!rc) {
        saved=next;
        if(r->action!=EMBK_WIFI_FORGET) {disconnect();profile=fresh;have_profile=want_join=1;}
        else if(profile.ssid_len==r->ssid_len && !memcmp(profile.ssid,r->ssid,r->ssid_len)) {
            want_join=have_profile=0;disconnect();erase(&profile,sizeof profile);
        }
    }
    erase(&fresh,sizeof fresh);erase(&next,sizeof next);return rc;
}
static void process_pending(void) {
    if(!busy) return;
    struct embk_wifi_request r=pending;erase(&pending,sizeof pending);
    request_error=apply_request(&r);erase(&r,sizeof r);completed_id=request_id;busy=0;
}
static void worker(void) {
    struct wifi_saved_profiles initial={0};load_saved(&initial);net_lock();
    saved=initial;erase(&initial,sizeof initial);
    if(saved.count) {profile=saved.items[0];have_profile=1;} else start_scan();
    for(;;) {
        process_pending();
        uint64_t now=time_get_us()/1000;const struct ax200_status *s=ax200_get_status();
        if(!scanning && want_join && have_profile && s->state==AX200_RADIO_READY && !s->rfkill && now>=retry_at &&
           (station.state==WIFI_STA_DOWN || station.state==WIFI_STA_FAILED)) {
            uint8_t channels[WIFI_CHANNELS_MAX];int count=ax200_channels(channels,sizeof channels);
            if(count>0) {
                ax200_leave();cleaned=0;
                if(wifi_station_join(&station,profile.ssid,profile.ssid_len,profile.pmk,channels,count,now)) retry_at=now+5000;
            } else retry_at=now+5000;
        }
        if(need_dhcp && station.state==WIFI_STA_UP && now>=dhcp_retry_at && !busy) {
            need_dhcp=0;net_unlock();int ok=net_dhcp();net_lock();
            if(station.state==WIFI_STA_UP && want_join) {
                if(ok) {
                    net_address_ready();
                    uint64_t renew=net_dhcp_renew_after_ms();
                    if(renew!=UINT64_MAX) {need_dhcp=1;dhcp_retry_at=time_get_us()/1000+renew;}
                } else {
                    if(!net_dhcp_lease_remaining_ms()) clear_address();
                    need_dhcp=1;dhcp_retry_at=time_get_us()/1000+10000;
                }
            } else clear_address();
        }
        net_wait();
    }
}
void wifi_start_worker(void) {if(active) worker_ready=process_create_kthread(worker,0)!=NULL;}
int wifi_snapshot(struct embk_wifi_info *out) {
    memset(out,0,sizeof(*out));out->version=EMBK_WIFI_VERSION;
    net_lock();const struct ax200_status *s=ax200_get_status();out->radio=s->state;
    if(active && worker_ready && s->state==AX200_RADIO_READY) out->flags|=EMBK_WIFI_READY;
    if(s->rfkill) out->flags|=EMBK_WIFI_BLOCKED;
    if(active && g_netif.up && g_netif.dhcp && g_netif.ip) out->flags|=EMBK_WIFI_ADDRESS;
    if(busy) out->flags|=EMBK_WIFI_BUSY;
    if(scanning || station.state==WIFI_STA_SCAN) out->flags|=EMBK_WIFI_SCANNING;
    if(want_join) out->flags|=EMBK_WIFI_AUTOCONNECT;
    out->state=station.state;out->error=station.error;
    if(active) {out->ip=g_netif.ip;out->netmask=g_netif.netmask;out->gateway=g_netif.gateway;out->dns=g_netif.dns;}
    out->request_id=request_id;out->completed_id=completed_id;out->request_error=request_error;
    out->ssid_len=profile.ssid_len;memcpy(out->ssid,profile.ssid,profile.ssid_len);
    out->scanned_ms=scanned_ms;out->network_count=network_count;memcpy(out->networks,networks,sizeof networks);
    out->saved_count=saved.count;
    for(unsigned i=0;i<saved.count;i++) {
        out->saved[i].ssid_len=saved.items[i].ssid_len;memcpy(out->saved[i].ssid,saved.items[i].ssid,saved.items[i].ssid_len);
        out->saved[i].security=EMBK_WIFI_SEC_WPA2;
    }
    net_unlock();return 0;
}
int wifi_request(const struct embk_wifi_request *r) {
    if(!wifi_request_valid(r)) return -EMBK_EINVAL;
    net_lock();int rc=0;const struct ax200_status *s=ax200_get_status();
    if(!active || !worker_ready || s->state!=AX200_RADIO_READY) rc=-EMBK_ENODEV;
    else if(busy) rc=-EMBK_EBUSY;
    else if(r->action!=EMBK_WIFI_DISCONNECT && r->action!=EMBK_WIFI_FORGET && s->rfkill) rc=-EMBK_EACCES;
    else if(r->action==EMBK_WIFI_SCAN && (scanning || (station.state!=WIFI_STA_DOWN && station.state!=WIFI_STA_FAILED))) rc=-EMBK_EBUSY;
    else {pending=*r;busy=1;request_error=0;if(++request_id>0x7fffffff) request_id=1;rc=request_id;}
    net_unlock();return rc;
}
int wifi_command(const char *cmd) {
    if(!cmd || strncmp(cmd,"wifi",4) || (cmd[4] && cmd[4]!=' ')) return 0;
    if(!strcmp(cmd,"wifi") || !strcmp(cmd,"wifi status")) {
        struct embk_wifi_info s;wifi_snapshot(&s);
        static const char *const states[]={"down","scanning","authenticating","associating","WPA2 handshake","authorized","failed"};
        static const char *const radios[]={"absent","missing firmware","invalid firmware","starting","firmware alive","NVM ready","radio ready","stopped"};
        kprintf("\nwifi: AX200 %s, radio=%s station=%s error=%u\n",radios[s.radio],
            (s.flags&EMBK_WIFI_READY)?((s.flags&EMBK_WIFI_BLOCKED)?"blocked":"enabled"):"unavailable",states[s.state],s.error);
        if(s.flags&EMBK_WIFI_ADDRESS) kprintf("wifi: ip %u.%u.%u.%u DNS %u.%u.%u.%u\n",IP_OCTETS(s.ip),IP_OCTETS(s.dns));
        else kprintf("wifi: no DHCP address\n");
    } else {
        struct embk_wifi_request r={.version=EMBK_WIFI_VERSION};
        if(!strcmp(cmd,"wifi reconnect")) r.action=EMBK_WIFI_RECONNECT;
        else if(!strcmp(cmd,"wifi disconnect")) r.action=EMBK_WIFI_DISCONNECT;
        else if(!strcmp(cmd,"wifi scan")) r.action=EMBK_WIFI_SCAN;
        if(r.action) kprintf("\nwifi: request %d\n",wifi_request(&r));
        else kprintf("\nusage: wifi status | wifi reconnect | wifi disconnect | wifi scan\n");
    }
    return 1;
}
