#include "control.h"
#include "wpa2_crypto.h"
#include "include/kstring.h"
static int same(const struct wifi_profile *a,const struct wifi_profile *b) {
    return a->ssid_len==b->ssid_len && !memcmp(a->ssid,b->ssid,a->ssid_len);
}
int wifi_store_decode(struct wifi_saved_profiles *out,const uint8_t *p,unsigned n) {
    memset(out,0,sizeof(*out));
    if(!p || n<8 || memcmp(p,"EWPF\1\0\0",7) || p[7]>EMBK_WIFI_SAVED || n!=8+65u*p[7]) return -1;
    struct wifi_saved_profiles tmp={0};tmp.count=p[7];
    for(unsigned i=0;i<tmp.count;i++) {
        const uint8_t *s=p+8+65*i;struct wifi_profile *v=&tmp.items[i];
        if(!s[0] || s[0]>32) goto bad;
        v->ssid_len=s[0];memcpy(v->ssid,s+1,32);memcpy(v->pmk,s+33,32);
        for(unsigned j=v->ssid_len;j<32;j++) if(v->ssid[j]) goto bad;
        for(unsigned j=0;j<i;j++) if(same(v,&tmp.items[j])) goto bad;
    }
    *out=tmp;
    {volatile uint8_t *q=(uint8_t *)&tmp;for(unsigned i=0;i<sizeof tmp;i++) q[i]=0;}
    return 0;
bad:
    {volatile uint8_t *q=(uint8_t *)&tmp;for(unsigned i=0;i<sizeof tmp;i++) q[i]=0;}
    return -1;
}
unsigned wifi_store_encode(const struct wifi_saved_profiles *s,uint8_t *p) {
    if(s->count>EMBK_WIFI_SAVED) return 0;
    unsigned n=8+65*s->count;memset(p,0,n);memcpy(p,"EWPF\1\0\0",7);p[7]=s->count;
    for(unsigned i=0;i<s->count;i++) {
        const struct wifi_profile *v=&s->items[i];
        if(!v->ssid_len || v->ssid_len>32) return 0;
        uint8_t *d=p+8+65*i;d[0]=v->ssid_len;memcpy(d+1,v->ssid,v->ssid_len);memcpy(d+33,v->pmk,32);
    }
    return n;
}
int wifi_request_valid(const struct embk_wifi_request *r) {
    if(r->version!=EMBK_WIFI_VERSION || r->ssid_len>32 || r->secret_len>64) return 0;
    switch(r->action) {
    case EMBK_WIFI_CONNECT:
        if(!r->ssid_len || r->secret_len<8) return 0;
        for(unsigned i=0;i<r->secret_len;i++) {
            unsigned c=r->secret[i];
            if(r->secret_len==64) {if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return 0;}
            else if(c<32 || c>126) return 0;
        }
        return 1;
    case EMBK_WIFI_RECONNECT:return !r->secret_len;
    case EMBK_WIFI_FORGET:return r->ssid_len && !r->secret_len;
    case EMBK_WIFI_DISCONNECT:case EMBK_WIFI_SCAN:return !r->ssid_len && !r->secret_len;
    default:return 0;
    }
}
int wifi_discovery_parse(struct embk_wifi_network *out,const uint8_t *p,unsigned n,uint8_t ch) {
    if(!p || n<36 || n>4096 || (p[0]!=0x80 && p[0]!=0x50) || (p[1]&0xc7) || (p[22]&15) ||
       (p[10]&1) || !memcmp(p+10,"\0\0\0\0\0\0",6) || memcmp(p+10,p+16,6) || !ch || ch>196 ||
       (!p[32]&&!p[33]) || (p[34]&3)!=1) return -1;
    struct embk_wifi_network v={0};v.channel=ch;
    unsigned ssid=0,rsn=0,ds=0;int supported=0;
    for(unsigned at=36;at<n;) {
        if(n-at<2 || p[at+1]>n-at-2) return -1;
        unsigned id=p[at],len=p[at+1];const uint8_t *d=p+at+2;at+=len+2;
        if(id==0) {if(ssid++ || !len || len>32) return -1;v.ssid_len=len;memcpy(v.ssid,d,len);}
        else if(id==48) {if(rsn++) return -1;supported=!wifi_rsn_ccmp_psk(d,len);}
        else if(id==3) {if(ds++ || len!=1 || d[0]!=ch) return -1;}
    }
    if(ssid!=1) return -1;
    v.security=(p[34]&16)?(supported?EMBK_WIFI_SEC_WPA2:EMBK_WIFI_SEC_OTHER):EMBK_WIFI_SEC_OPEN;
    *out=v;return 0;
}
