#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/wifi/control.h"
void kprintf(const char *fmt,...) {(void)fmt;}
static void profiles(void) {
    struct wifi_saved_profiles a={0},b;uint8_t bytes[WIFI_STORE_BYTES+1];
    a.count=2;a.items[0].ssid_len=4;memcpy(a.items[0].ssid,"IEEE",4);
    a.items[1].ssid_len=3;memcpy(a.items[1].ssid,"a\0b",3);
    memset(a.items[0].pmk,0x11,32);memset(a.items[1].pmk,0x22,32);
    unsigned n=wifi_store_encode(&a,bytes);assert(n==138);
    assert(!wifi_store_decode(&b,bytes,n) && !memcmp(&a,&b,sizeof a));
    for(unsigned i=0;i<n;i++) assert(wifi_store_decode(&b,bytes,i));
    assert(wifi_store_decode(&b,bytes,n+1));
    bytes[7]=9;assert(wifi_store_decode(&b,bytes,n));bytes[7]=2;
    bytes[8]=0;assert(wifi_store_decode(&b,bytes,n));bytes[8]=4;
    bytes[8+1+31]=1;assert(wifi_store_decode(&b,bytes,n));bytes[8+1+31]=0;
    memcpy(bytes+8+65,bytes+8,65);assert(wifi_store_decode(&b,bytes,n));
    a.count=0;n=wifi_store_encode(&a,bytes);assert(n==8 && !wifi_store_decode(&b,bytes,n) && !b.count);
}
static void requests(void) {
    struct embk_wifi_request r={.version=1,.action=EMBK_WIFI_CONNECT,.ssid_len=4,.secret_len=8};
    memcpy(r.ssid,"IEEE",4);memcpy(r.secret,"password",8);assert(wifi_request_valid(&r));
    r.version=2;assert(!wifi_request_valid(&r));r.version=1;
    r.secret_len=7;assert(!wifi_request_valid(&r));r.secret_len=65;assert(!wifi_request_valid(&r));
    r.secret_len=64;memset(r.secret,'f',64);assert(wifi_request_valid(&r));r.secret[63]='g';assert(!wifi_request_valid(&r));
    r.secret_len=8;r.secret[0]=31;assert(!wifi_request_valid(&r));
    r.action=EMBK_WIFI_RECONNECT;r.secret_len=0;assert(wifi_request_valid(&r));
    r.action=EMBK_WIFI_FORGET;r.ssid_len=0;assert(!wifi_request_valid(&r));r.ssid_len=32;assert(wifi_request_valid(&r));
    r.action=EMBK_WIFI_SCAN;assert(!wifi_request_valid(&r));r.ssid_len=0;assert(wifi_request_valid(&r));
    r.action=999;assert(!wifi_request_valid(&r));
}
static void discovery(void) {
    uint8_t p[128]={0};struct embk_wifi_network out;
    p[0]=0x80;memset(p+4,255,6);memcpy(p+10,"\x02\x03\x04\x05\x06\x07",6);memcpy(p+16,p+10,6);
    p[32]=100;p[34]=1;p[36]=0;p[37]=4;memcpy(p+38,"test",4);
    assert(!wifi_discovery_parse(&out,p,42,1) && out.ssid_len==4 && out.security==EMBK_WIFI_SEC_OPEN);
    for(unsigned n=0;n<42;n++) assert(wifi_discovery_parse(&out,p,n,1));
    p[34]|=16;assert(!wifi_discovery_parse(&out,p,42,1) && out.security==EMBK_WIFI_SEC_OTHER);
    const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    memcpy(p+42,rsn,sizeof rsn);unsigned n=42+sizeof rsn;
    assert(!wifi_discovery_parse(&out,p,n,1) && out.security==EMBK_WIFI_SEC_WPA2);
    p[n]=0;p[n+1]=1;p[n+2]='x';assert(wifi_discovery_parse(&out,p,n+3,1));
    p[n]=3;p[n+1]=1;p[n+2]=6;assert(wifi_discovery_parse(&out,p,n+3,1));
    p[n+2]=1;assert(!wifi_discovery_parse(&out,p,n+3,1));
    p[1]=0x40;assert(wifi_discovery_parse(&out,p,n,1));p[1]=0;
    p[10]|=1;assert(wifi_discovery_parse(&out,p,n,1));
}
int main(void) {profiles();requests();discovery();puts("wifi control: profiles, request validation and hostile beacon bounds OK");}
