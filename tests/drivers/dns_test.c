#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/net.h"
struct netif g_netif;
static uint8_t query[300];static unsigned length,mode,locks;
void net_lock(void) {}
void net_unlock(void) {}
void net_service_lock(void) {assert(!locks);locks=1;}
void net_service_unlock(void) {assert(locks);locks=0;}
void udp_arm_peer(uint16_t p,uint32_t ip,uint16_t peer) {assert(p==49152 && ip==g_netif.dns && peer==53);}
int net_send_udp(uint32_t src,uint16_t sp,uint32_t dst,uint16_t dp,const void *p,uint32_t n) {
    assert(src==g_netif.ip && sp==49152 && dst==g_netif.dns && dp==53 && n<=sizeof query);
    memcpy(query,p,n);length=n;return 0;
}
int udp_collect(uint8_t *out,uint32_t cap,uint32_t *from) {
    (void)from;assert(cap>=length+16);memcpy(out,query,length);out[2]=0x81;out[3]=0x80;out[6]=0;out[7]=1;
    const uint8_t answer[]={0xc0,0x0c,0,1,0,1,0,0,0,60,0,4,192,0,2,80};
    memcpy(out+length,answer,sizeof answer);unsigned n=length+sizeof answer;
    if(mode==1) out[2]&=~0x80;           /* request disguised as answer */
    if(mode==2) out[2]|=2;               /* truncated UDP reply */
    if(mode==3) out[3]|=3;               /* NXDOMAIN */
    if(mode==4) out[0]^=1;               /* different transaction */
    if(mode==5) out[length+5]=3;         /* wrong RR class */
    if(mode==6) out[length+1]=255;       /* invalid compression pointer */
    if(mode==7) n--;                    /* truncated RDATA */
    if(mode==8) out[12]=0x40;           /* reserved label type */
    if(mode==9) out[5]=2;               /* wrong question count */
    return n;
}
int main(void) {
    g_netif.ip=IPV4(10,0,2,15);g_netif.dns=IPV4(10,0,2,3);uint32_t address=0;
    assert(net_resolve("example.com",&address) && address==IPV4(192,0,2,80));
    for(mode=1;mode<=9;mode++) assert(!net_resolve("example.com",&address));
    assert(!locks);puts("DNS: source-filter contract, response flags/ID, class, compression, truncation and malformed questions passed");
}
