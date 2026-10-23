/* A small infrastructure station: passive scan, legacy open authentication,
 * association, WPA2/CCMP and Ethernet encapsulation. No ad-hoc/WEP/TKIP/PMF,
 * HT aggregation, fragments or A-MSDUs are negotiated. */
#include "station.h"
#include "wpa2_crypto.h"
#include "include/kstring.h"
#define SCAN_DWELL 250
#define RESPONSE_MS 1000
#define ASSOCIATION_TRIES 3
#define LINK_IDLE_MS 30000
static const uint8_t broadcast[6]={255,255,255,255,255,255};
static const uint8_t rates[]={2,4,11,22,12,18,24,36,48,72,96,108};
static const uint8_t snap[6]={0xaa,0xaa,3,0,0,0};
static unsigned le16(const uint8_t *p) {return p[0]+(unsigned)p[1]*256;}
static void put16(uint8_t *p,unsigned v) {p[0]=v;p[1]=v>>8;}
static void erase(void *p,unsigned n) {volatile uint8_t *v=p;while(n--) *v++=0;}
static int unicast(const uint8_t *p) {
    return !(p[0]&1) && (p[0]|p[1]|p[2]|p[3]|p[4]|p[5]);
}
static void clear_keys(struct wifi_station *s) {
    wifi_wpa2_stop(&s->wpa);wifi_ccmp_clear(&s->pair);
    for(unsigned i=0;i<4;i++) wifi_ccmp_clear(&s->group[i]);
}
static void failed(struct wifi_station *s,enum wifi_station_error why) {
    s->state=WIFI_STA_FAILED;s->error=why;clear_keys(s);s->ops.link(s->arg,0);
}
static void header(struct wifi_station *s,uint8_t *p,unsigned fc,const uint8_t *dst,const uint8_t *a3) {
    memset(p,0,24);put16(p,fc);memcpy(p+4,dst,6);memcpy(p+10,s->mac,6);
    memcpy(p+16,a3,6);put16(p+22,(s->sequence++&4095)<<4);
}
static int send_data(struct wifi_station *s,const uint8_t *dst,unsigned type,
    const uint8_t *data,unsigned n,int protected_frame) {
    if(n>1500) return -1;
    uint8_t plain[WIFI_MPDU_MAX],encrypted[WIFI_MPDU_MAX];
    header(s,plain,0x0108,s->bss.ap,dst);memcpy(plain+24,snap,6);
    plain[30]=type>>8;plain[31]=type;memcpy(plain+32,data,n);
    if(!protected_frame) return s->ops.tx(s->arg,plain,n+32);
    int count=wifi_ccmp_seal(&s->pair,plain,n+32,encrypted,sizeof encrypted);
    return count<0?-1:s->ops.tx(s->arg,encrypted,count);
}
static int eapol_send(void *arg,const uint8_t *p,unsigned n,int prot) {
    struct wifi_station *s=arg;return send_data(s,s->bss.ap,0x888e,p,n,prot);
}
static int random_bytes(void *arg,uint8_t *p,unsigned n) {
    struct wifi_station *s=arg;return s->ops.random(s->arg,p,n);
}
static int install_keys(void *arg,const struct wifi_wpa2_keys *k) {
    struct wifi_station *s=arg;
    if(k->gtk_id>3 || k->group_rsc>WIFI_CCMP_PN_MAX) return -1;
    if(k->pairwise && wifi_ccmp_install(&s->pair,k->tk,0,0)) return -1;
    if(k->group) {
        struct wifi_ccmp_key *dst=&s->group[k->gtk_id];
        if(wifi_ccmp_install(dst,k->gtk,k->gtk_id,k->group_rsc)) return -1;
        /* Moving the same GTK to another index must not rewind any replay
         * queue. The key index is not part of the CCMP nonce. */
        for(unsigned i=0;i<4;i++) if(i!=k->gtk_id && s->group[i].installed &&
            !memcmp(s->group[i].tk,k->gtk,16)) {
            for(unsigned j=0;j<17;j++) if(dst->rx_pn[j]<s->group[i].rx_pn[j])
                dst->rx_pn[j]=s->group[i].rx_pn[j];
            wifi_ccmp_clear(&s->group[i]);
        }
    }
    return 0;
}
static void set_port(void *arg,int up) {
    struct wifi_station *s=arg;
    if(up) {s->state=WIFI_STA_UP;s->last_rx=s->now;}
    s->ops.link(s->arg,up);
}
static const struct wifi_wpa2_ops security_ops={random_bytes,eapol_send,install_keys,set_port};
int wifi_station_init(struct wifi_station *s,const struct wifi_radio_ops *ops,void *arg,const uint8_t mac[6]) {
    if(!s || !ops || !ops->tune || !ops->prepare || !ops->associate || !ops->tx ||
       !ops->random || !ops->ether || !ops->link || !mac || !unicast(mac)) return -1;
    memset(s,0,sizeof(*s));s->ops=*ops;s->arg=arg;memcpy(s->mac,mac,6);return 0;
}
void wifi_station_disconnect(struct wifi_station *s) {
    if(!s) return;
    clear_keys(s);erase(s->pmk,sizeof s->pmk);erase(s->ssid,sizeof s->ssid);
    memset(&s->bss,0,sizeof s->bss);s->ssid_len=0;s->state=WIFI_STA_DOWN;s->error=WIFI_ERR_NONE;
    if(s->ops.link) s->ops.link(s->arg,0);
}
int wifi_station_join(struct wifi_station *s,const uint8_t *ssid,unsigned sn,
    const uint8_t pmk[32],const uint8_t *channels,unsigned cn,uint64_t now) {
    if(!s || !s->ops.tune || !ssid || !sn || sn>32 || !pmk || !channels || !cn || cn>WIFI_CHANNELS_MAX) return -1;
    for(unsigned i=0;i<cn;i++) if(!channels[i] || channels[i]>196) return -1;
    wifi_station_disconnect(s);memcpy(s->ssid,ssid,sn);s->ssid_len=sn;memcpy(s->pmk,pmk,32);
    memcpy(s->channels,channels,cn);s->channel_count=cn;s->channel_index=0;s->sweeps=0;
    s->state=WIFI_STA_SCAN;s->now=now;s->deadline=now+SCAN_DWELL;
    if(s->ops.tune(s->arg,channels[0])) {failed(s,WIFI_ERR_RADIO);return -1;}
    return 0;
}
static int rate_supported(unsigned r,unsigned channel) {
    for(unsigned i=channel<=14?0:4;i<sizeof rates;i++) if(rates[i]==(r&127)) return 1;
    return 0;
}
/* Parse all IEs, not just the first SSID/RSN. Duplicates and truncations must
 * not allow conflicting security, SSID or basic-rate information to pass. */
static int bss_ies(struct wifi_station *s,struct wifi_bss *b,const uint8_t *p,unsigned n,int beacon) {
    unsigned at=0,ssid=0,rsn=0,rate_ie=0,ext_rates=0,ds=0,tim=0;uint8_t list[255];unsigned nr=0;
    while(at<n) {
        if(n-at<2 || p[at+1]>n-at-2) return -1;
        const uint8_t *ie=p+at;unsigned len=ie[1];at+=len+2;
        if(ie[0]==0) {
            if(ssid++ || len!=s->ssid_len || memcmp(ie+2,s->ssid,len)) return -1;
        } else if(ie[0]==48) {
            if(rsn++ || wifi_rsn_ccmp_psk(ie+2,len)) return -1;
            if(beacon) {memcpy(b->rsn,ie,len+2);b->rsn_len=len+2;}
            else if(len+2!=b->rsn_len || memcmp(ie,b->rsn,len+2)) return -1;
        } else if(ie[0]==1 || ie[0]==50) {
            if((ie[0]==1 && (rate_ie++ || !len || len>8)) ||
               (ie[0]==50 && ext_rates++) || nr+len>sizeof list) return -1;
            memcpy(list+nr,ie+2,len);nr+=len;
        } else if(ie[0]==3) {
            if(ds++ || len!=1 || ie[2]!=b->channel) return -1;
        } else if(ie[0]==5 && beacon) {
            if(tim++ || len<4 || !ie[3] || ie[2]>=ie[3]) return -1;
            b->dtim_count=ie[2];b->dtim_period=ie[3];
        }
    }
    if(!rate_ie || (beacon && (ssid!=1 || rsn!=1))) return -1;
    b->rates_len=0;
    for(unsigned i=0;i<nr;i++) {
        int supported=rate_supported(list[i],b->channel);
        if(!supported && (list[i]&128)) return -1;
        if(!supported) continue;
        unsigned j;for(j=0;j<b->rates_len;j++) if((b->rates[j]&127)==(list[i]&127)) break;
        if(j<b->rates_len) b->rates[j]|=list[i]&128;
        else {if(b->rates_len==sizeof b->rates) return -1;b->rates[b->rates_len++]=list[i];}
    }
    return b->rates_len?0:-1;
}
static int send_auth(struct wifi_station *s) {
    uint8_t p[30];header(s,p,0x00b0,s->bss.ap,s->bss.ap);
    memset(p+24,0,6);put16(p+26,1);return s->ops.tx(s->arg,p,sizeof p);
}
static int send_assoc(struct wifi_station *s) {
    uint8_t p[128];header(s,p,0,s->bss.ap,s->bss.ap);
    put16(p+24,s->bss.capabilities&0x0431);put16(p+26,10);
    unsigned at=28;p[at++]=0;p[at++]=s->ssid_len;memcpy(p+at,s->ssid,s->ssid_len);at+=s->ssid_len;
    unsigned first=s->bss.rates_len<8?s->bss.rates_len:8;
    p[at++]=1;p[at++]=first;memcpy(p+at,s->bss.rates,first);at+=first;
    if(s->bss.rates_len>first) {
        unsigned rest=s->bss.rates_len-first;p[at++]=50;p[at++]=rest;
        memcpy(p+at,s->bss.rates+first,rest);at+=rest;
    }
    memcpy(p+at,wifi_wpa2_station_rsn,sizeof wifi_wpa2_station_rsn);at+=sizeof wifi_wpa2_station_rsn;
    return s->ops.tx(s->arg,p,at);
}
static void management(struct wifi_station *s,const uint8_t *p,unsigned n,uint8_t channel) {
    if((p[1]&0xc7) || (p[22]&15) || !unicast(p+10) || memcmp(p+10,p+16,6)) return;
    if(s->state==WIFI_STA_SCAN && (p[0]==0x80 || p[0]==0x50)) {
        if(n<36 || (memcmp(p+4,broadcast,6) && memcmp(p+4,s->mac,6)) ||
           channel!=s->channels[s->channel_index] || !le16(p+32) || (le16(p+34)&0x13)!=0x11) return;
        struct wifi_bss b={0};b.channel=channel;memcpy(b.ap,p+10,6);
        memcpy(b.ssid,s->ssid,s->ssid_len);b.ssid_len=s->ssid_len;
        b.capabilities=le16(p+34);b.beacon_interval=le16(p+32);
        memcpy(&b.tsf,p+24,8);b.dtim_period=1;
        if(bss_ies(s,&b,p+36,n-36,1)) return;
        s->bss=b;
        if(s->ops.prepare(s->arg,&s->bss)) {failed(s,WIFI_ERR_RADIO);return;}
        s->state=WIFI_STA_AUTH;s->attempts=1;s->deadline=s->now+RESPONSE_MS;
        if(send_auth(s)) failed(s,WIFI_ERR_RADIO);
        return;
    }
    if(memcmp(p+10,s->bss.ap,6) || channel!=s->bss.channel) return;
    if(p[0]==0x80 && n>=36 && !memcmp(p+4,broadcast,6) && s->state==WIFI_STA_UP) {
        struct wifi_bss b=s->bss;
        if(le16(p+32) && (le16(p+34)&0x13)==0x11 && !bss_ies(s,&b,p+36,n-36,1) &&
           b.rsn_len==s->bss.rsn_len && !memcmp(b.rsn,s->bss.rsn,b.rsn_len)) s->last_rx=s->now;
        return;
    }
    if(memcmp(p+4,s->mac,6)) return;
    if((p[0]==0xc0 || p[0]==0xa0) && n>=26 && s->state>=WIFI_STA_AUTH && s->state<=WIFI_STA_UP) {
        failed(s,WIFI_ERR_DISCONNECTED);return;
    }
    if(s->state==WIFI_STA_AUTH && p[0]==0xb0 && n>=30) {
        if(le16(p+24)!=0 || le16(p+26)!=2) return;
        if(le16(p+28)) {failed(s,WIFI_ERR_AUTH);return;}
        s->state=WIFI_STA_ASSOC;s->attempts=1;s->deadline=s->now+RESPONSE_MS;
        if(send_assoc(s)) failed(s,WIFI_ERR_RADIO);
    } else if(s->state==WIFI_STA_ASSOC && p[0]==0x10 && n>=30) {
        if(le16(p+26)) {failed(s,WIFI_ERR_ASSOC);return;}
        struct wifi_bss b=s->bss;unsigned aid=le16(p+28)&0x3fff;
        if(!aid || aid>2007 || (le16(p+24)&0x13)!=0x11 || bss_ies(s,&b,p+30,n-30,0)) {
            failed(s,WIFI_ERR_ASSOC);return;
        }
        b.aid=aid;s->bss=b;
        if(s->ops.associate(s->arg,&s->bss)) {failed(s,WIFI_ERR_RADIO);return;}
        s->state=WIFI_STA_HANDSHAKE;
        if(wifi_wpa2_start(&s->wpa,&security_ops,s,s->mac,s->bss.ap,s->pmk,s->bss.rsn,s->bss.rsn_len,s->now))
            failed(s,WIFI_ERR_SECURITY);
    }
}
void wifi_station_rx(struct wifi_station *s,const uint8_t *p,unsigned n,uint8_t channel,uint64_t now) {
    if(!s || !p || n<24 || n>WIFI_MPDU_MAX || s->state==WIFI_STA_DOWN || s->state==WIFI_STA_FAILED) return;
    s->now=now;
    if((p[0]&0x0f)==0) {management(s,p,n,channel);return;}
    if((s->state!=WIFI_STA_HANDSHAKE && s->state!=WIFI_STA_UP) ||
       (p[0]!=0x08 && p[0]!=0x88) || (p[1]&0x87)!=2 || (p[22]&15) ||
       memcmp(p+10,s->bss.ap,6) || channel!=s->bss.channel || !unicast(p+16) ||
       (!(p[4]&1) && memcmp(p+4,s->mac,6))) return;
    unsigned hl=(p[0]&0x80)?26:24;
    if(n<hl || (hl==26 && ((p[24]&0x80) || p[25]))) return;
    uint8_t plain[WIFI_MPDU_MAX];int encrypted=(p[1]&0x40)!=0;
    if(encrypted) {
        if(n<hl+16) return;
        unsigned id=p[hl+3]>>6;
        if(!(p[4]&1) && id) return;
        struct wifi_ccmp_key *key=(p[4]&1)?&s->group[id]:&s->pair;
        int count=wifi_ccmp_open(key,p,n,plain,sizeof plain);if(count<0) return;
        p=plain;n=count;
    }
    if(n<hl+8 || memcmp(p+hl,snap,6)) return;
    unsigned type=(unsigned)p[hl+6]*256+p[hl+7];
    if(type==0x888e) {
        if((p[4]&1) || memcmp(p+16,s->bss.ap,6)) return;
        /* Initial EAPOL may be unencrypted; a GTK rekey uses the established
         * pairwise cipher. M3 retransmissions remain valid without CCMP. */
        const uint8_t *e=p+hl+8;unsigned en=n-hl-8;
        if(!encrypted && en>=7 && !(e[6]&8)) return;
        int rc=wifi_wpa2_rx(&s->wpa,e,en,now);
        if(rc<0) failed(s,WIFI_ERR_SECURITY);
        else if(rc && encrypted) s->last_rx=now;
        return;
    }
    if(s->state!=WIFI_STA_UP || !s->wpa.authorized || !encrypted || n-hl-8>1500) return;
    uint8_t eth[1514];memcpy(eth,p+4,6);memcpy(eth+6,p+16,6);
    eth[12]=type>>8;eth[13]=type;memcpy(eth+14,p+hl+8,n-hl-8);
    s->last_rx=now;s->ops.ether(s->arg,eth,n-hl+6);
}
void wifi_station_tick(struct wifi_station *s,uint64_t now) {
    if(!s) return;
    s->now=now;
    if(s->state==WIFI_STA_HANDSHAKE) {
        wifi_wpa2_tick(&s->wpa,now);
        if(s->wpa.state==WIFI_WPA2_FAILED) failed(s,WIFI_ERR_SECURITY);
    } else if(s->state==WIFI_STA_UP) {
        if(now-s->last_rx>=LINK_IDLE_MS) failed(s,WIFI_ERR_TIMEOUT);
    } else if(now>=s->deadline && s->state==WIFI_STA_SCAN) {
        if(++s->channel_index==s->channel_count) {
            s->channel_index=0;if(++s->sweeps==2) {failed(s,WIFI_ERR_NOT_FOUND);return;}
        }
        if(s->ops.tune(s->arg,s->channels[s->channel_index])) {failed(s,WIFI_ERR_RADIO);return;}
        s->deadline=now+SCAN_DWELL;
    } else if(now>=s->deadline && (s->state==WIFI_STA_AUTH || s->state==WIFI_STA_ASSOC)) {
        if(s->attempts++>=ASSOCIATION_TRIES) {failed(s,WIFI_ERR_TIMEOUT);return;}
        s->deadline=now+RESPONSE_MS;
        if(s->state==WIFI_STA_AUTH?send_auth(s):send_assoc(s)) failed(s,WIFI_ERR_RADIO);
    }
}
int wifi_station_tx(struct wifi_station *s,const uint8_t *p,unsigned n) {
    if(!s || !p || n<14 || n>1514 || s->state!=WIFI_STA_UP || !s->wpa.authorized ||
       memcmp(p+6,s->mac,6) || (p[12]==0x88 && p[13]==0x8e)) return -1;
    return send_data(s,p,(unsigned)p[12]*256+p[13],p+14,n-14,1);
}
