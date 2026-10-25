#include "profile.h"
#include "include/kstring.h"
static int hex(unsigned c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
int wifi_profile_parse(struct wifi_profile *out,const uint8_t *p,unsigned n) {
    if(!out) return -1;
    memset(out,0,sizeof(*out));
    if(!p || n<85 || n>147 || memcmp(p,"ssid_hex=",9)) return -1;
    unsigned at=9;uint8_t ssid[32],pmk[32];unsigned sn=0;
    while(at<n && p[at]!='\n') {
        if(n-at<2 || sn==32) return -1;
        int a=hex(p[at]),b=hex(p[at+1]);if(a<0 || b<0) return -1;
        ssid[sn++]=(a<<4)|b;at+=2;
    }
    if(!sn || n-at!=74 || p[at++]!='\n' || memcmp(p+at,"psk_hex=",8)) return -1;
    at+=8;
    for(unsigned i=0;i<32;i++,at+=2) {
        int a=hex(p[at]),b=hex(p[at+1]);if(a<0 || b<0) return -1;
        pmk[i]=(a<<4)|b;
    }
    if(p[at]!='\n') return -1;
    memcpy(out->ssid,ssid,sn);out->ssid_len=sn;memcpy(out->pmk,pmk,32);
    volatile uint8_t *q=pmk;for(unsigned i=0;i<32;i++) q[i]=0;return 0;
}
