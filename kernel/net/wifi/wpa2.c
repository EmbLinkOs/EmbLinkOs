/* IEEE 802.11 RSNA supplicant: WPA2-Personal, CCMP-128, SHA1 descriptor v2.
 * The controlled port, key installation and retransmission states are separate:
 * an authenticated retransmitted M3/group M1 can resend a response but cannot
 * reinstall a key (and therefore cannot reset CCMP packet numbers). */
#include "wpa2.h"
#include "wpa2_crypto.h"
#include "crypto/sha1.h"
#include "include/kstring.h"

#define PAIR_M1 0x008a
#define PAIR_M2 0x010a
#define PAIR_M3 0x13ca
#define PAIR_M4 0x030a
#define GROUP_M1 0x1382
#define GROUP_M2 0x0302
#define KEY_FIXED 99
#define MIC_AT 81
#define RETRY_MS 1000
#define MAX_ATTEMPTS 4
const uint8_t wifi_wpa2_station_rsn[22] = {
    48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0
};
static void erase(void *p,unsigned n) {volatile uint8_t *v=p;while(n--) *v++=0;}
static unsigned be16(const uint8_t *p) {return (unsigned)p[0]*256+p[1];}
static void put16(uint8_t *p,unsigned v) {p[0]=v>>8;p[1]=v;}
static uint64_t be64(const uint8_t *p) {
    uint64_t v=0;for(unsigned i=0;i<8;i++) v=(v<<8)|p[i];return v;
}
static int equal(const uint8_t *a,const uint8_t *b,unsigned n) {
    unsigned d=0;for(unsigned i=0;i<n;i++) d|=a[i]^b[i];return d==0;
}
static int nonzero(const uint8_t *p,unsigned n) {
    unsigned v=0;for(unsigned i=0;i<n;i++) v|=p[i];return v!=0;
}
static void port(struct wifi_wpa2 *s,int up) {
    if(s->authorized==up) return;
    s->authorized=up;s->ops.port(s->arg,up);
}
void wifi_wpa2_stop(struct wifi_wpa2 *s) {
    if(!s) return;
    if(s->authorized && s->ops.port) s->ops.port(s->arg,0);
    erase(s,sizeof(*s));
}
static int fail(struct wifi_wpa2 *s) {
    port(s,0);s->state=WIFI_WPA2_FAILED;s->reply_len=0;
    erase(s->ptk,sizeof s->ptk);erase(s->pmk,sizeof s->pmk);
    erase(s->snonce,sizeof s->snonce);return -1;
}
int wifi_wpa2_start(struct wifi_wpa2 *s,const struct wifi_wpa2_ops *ops,void *arg,
    const uint8_t sta[6],const uint8_t ap[6],const uint8_t pmk[32],
    const uint8_t *rsn,unsigned rn,uint64_t now) {
    if(!s || !ops || !ops->random || !ops->send || !ops->install || !ops->port ||
       !sta || !ap || !pmk || !rsn || rn<2 || rn>WIFI_RSN_MAX ||
       rsn[0]!=48 || rsn[1]!=rn-2 || wifi_rsn_ccmp_psk(rsn+2,rn-2)) return -1;
    /* A fresh connection must first close the previous port and remove its
     * cipher state. The adapter calls stop before reusing this object. */
    memset(s,0,sizeof(*s));s->ops=*ops;s->arg=arg;
    memcpy(s->sta,sta,6);memcpy(s->ap,ap,6);memcpy(s->pmk,pmk,32);
    memcpy(s->ap_rsn,rsn,rn);s->ap_rsn_len=rn;
    s->state=WIFI_WPA2_M1;s->deadline=now+RETRY_MS*MAX_ATTEMPTS;
    s->ops.port(s->arg,0);return 0;
}
static int send_reply(struct wifi_wpa2 *s,uint64_t now) {
    s->deadline=now+RETRY_MS;s->attempts++;
    int rc=s->ops.send(s->arg,s->reply,s->reply_len,s->reply_protected);
    if(rc==0 && s->state==WIFI_WPA2_M4) {
        s->state=WIFI_WPA2_CONNECTED;port(s,1);
    }
    return rc;
}
static void response(struct wifi_wpa2 *s,const uint8_t *in,unsigned info,int with_rsn) {
    unsigned data=with_rsn?sizeof wifi_wpa2_station_rsn:0;
    s->reply_len=KEY_FIXED+data;memset(s->reply,0,s->reply_len);
    s->reply[0]=in[0];s->reply[1]=3;put16(s->reply+2,s->reply_len-4);
    s->reply[4]=2;put16(s->reply+5,info);
    /* RSN message 2/4 and group message 2 use a zero key-length field. */
    memcpy(s->reply+9,in+9,8);
    if(with_rsn) {
        memcpy(s->reply+17,s->snonce,32);
        memcpy(s->reply+KEY_FIXED,wifi_wpa2_station_rsn,data);
    }
    put16(s->reply+97,data);
    wifi_wpa2_mic(s->ptk,s->reply,s->reply_len,s->reply+MIC_AT);
}
static int valid_mic(struct wifi_wpa2 *s,const uint8_t *p,unsigned n) {
    uint8_t copy[WIFI_EAPOL_MAX],mic[16];
    memcpy(copy,p,n);memset(copy+MIC_AT,0,16);
    wifi_wpa2_mic(s->ptk,copy,n,mic);
    int ok=equal(mic,p+MIC_AT,16);erase(copy,n);erase(mic,16);return ok;
}
/* Key data is authenticated before parsing. A padding element consists of DD
 * followed by zeros; do not interpret it as a truncated GTK KDE. */
static int key_data(struct wifi_wpa2 *s,const uint8_t *p,unsigned n,int pair,
    struct wifi_wpa2_keys *keys) {
    uint8_t plain[256];int count=wifi_wpa2_unwrap(s->ptk+16,p,n,plain,sizeof plain);
    if(count<0) return -1;
    unsigned at=0,rn=(unsigned)count;int rsn_seen=0,gtk_seen=0,result=-1;
    while(at<rn) {
        if(plain[at]==221 && !nonzero(plain+at+1,rn-at-1)) break;
        if(rn-at<2 || (unsigned)plain[at+1]>rn-at-2) goto out;
        unsigned len=plain[at+1]+2;const uint8_t *ie=plain+at;
        if(ie[0]==48) {
            if(rsn_seen++ || len!=s->ap_rsn_len || !equal(ie,s->ap_rsn,len)) goto out;
        } else if(ie[0]==221 && len>=6 && !ie[2] && ie[3]==15 && ie[4]==172 && ie[5]==1) {
            if(gtk_seen++ || len!=24 || (ie[6]&~7U) || ie[7]) goto out;
            keys->gtk_id=ie[6]&3;memcpy(keys->gtk,ie+8,16);keys->group=1;
        }
        at+=len;
    }
    if(gtk_seen==1 && (!pair || rsn_seen==1)) result=0;
out:
    erase(plain,sizeof plain);return result;
}
int wifi_wpa2_rx(struct wifi_wpa2 *s,const uint8_t *p,unsigned n,uint64_t now) {
    if(!s || !p || s->state==WIFI_WPA2_IDLE || s->state==WIFI_WPA2_FAILED ||
       n<KEY_FIXED || n>WIFI_EAPOL_MAX || p[0]<1 || p[0]>2 || p[1]!=3 ||
       be16(p+2)!=n-4 || p[4]!=2 || be16(p+97)!=n-KEY_FIXED) return 0;
    unsigned info=be16(p+5),data=n-KEY_FIXED;
    uint64_t replay=be64(p+9);uint8_t digest[20];sha1(p,n,digest);
    if(s->have_replay && replay<s->replay) return 0;
    if(s->have_replay && replay==s->replay) {
        if(!s->reply_len || !equal(digest,s->last_digest,20)) return 0;
        /* Do not extend a stalled handshake forever with duplicate M1/M3. */
        if(s->state!=WIFI_WPA2_CONNECTED && s->attempts>=MAX_ATTEMPTS) return 0;
        (void)send_reply(s,now);return 1;
    }
    if(info==PAIR_M1) {
        if((s->state!=WIFI_WPA2_M1 && s->state!=WIFI_WPA2_M3) ||
           be16(p+7)!=16 || !nonzero(p+17,32)) return 0;
        /* M1 is unauthenticated: only accept it during initial authentication,
         * never let a forged M1 replace keys on an established link. */
        if(s->ops.random(s->arg,s->snonce,32) || !nonzero(s->snonce,32)) return fail(s);
        memcpy(s->anonce,p+17,32);
        wifi_wpa2_ptk(s->pmk,s->sta,s->ap,s->snonce,s->anonce,s->ptk);
        response(s,p,PAIR_M2,1);s->reply_protected=0;
        s->state=WIFI_WPA2_M3;
    } else {
        int pair=info==PAIR_M3;
        if((!pair && info!=GROUP_M1) || be16(p+7)!=16 || data<24 ||
           (pair && s->state!=WIFI_WPA2_M3) ||
           (!pair && s->state!=WIFI_WPA2_CONNECTED) ||
           (pair && !equal(p+17,s->anonce,32)) || !valid_mic(s,p,n)) return 0;
        struct wifi_wpa2_keys keys={0};
        if(p[71] || p[72]) return 0; /* CCMP RSC is 48 bits, little endian */
        for(unsigned i=0;i<6;i++) keys.group_rsc|=(uint64_t)p[65+i]<<(i*8);
        if(key_data(s,p+KEY_FIXED,data,pair,&keys)) {erase(&keys,sizeof keys);return 0;}
        keys.pairwise=pair;if(pair) memcpy(keys.tk,s->ptk+32,16);
        int rc=s->ops.install(s->arg,&keys);erase(&keys,sizeof keys);
        if(rc) return fail(s);
        response(s,p,pair?PAIR_M4:GROUP_M2,0);
        /* M4 of the initial handshake is sent without link encryption. Group
         * rekey responses travel through the already-established CCMP key. */
        s->reply_protected=!pair;
        if(pair) s->state=WIFI_WPA2_M4;
    }
    s->have_replay=1;s->replay=replay;memcpy(s->last_digest,digest,20);s->attempts=0;
    (void)send_reply(s,now);return 1;
}
void wifi_wpa2_tick(struct wifi_wpa2 *s,uint64_t now) {
    if(!s || s->state==WIFI_WPA2_IDLE || s->state==WIFI_WPA2_FAILED ||
       s->state==WIFI_WPA2_CONNECTED || now<s->deadline) return;
    if(!s->reply_len || s->attempts>=MAX_ATTEMPTS) {(void)fail(s);return;}
    (void)send_reply(s,now);
}
