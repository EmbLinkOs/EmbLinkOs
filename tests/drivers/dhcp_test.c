/* Exercise the actual DHCP client with deliberately hostile server replies. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/net.h"
struct netif g_netif;
static uint8_t request[512];static unsigned request_len,mode,sends,arp_resets;
static uint64_t ticks;
void net_lock(void) {}
void net_unlock(void) {}
uint64_t net_ticks(void) {return ticks;}
void arp_reset(void) {arp_resets++;}
static int service;
void net_service_lock(void) {assert(!service);service=1;}
void net_service_unlock(void) {assert(service);service=0;}
void udp_arm(uint16_t p) {assert(p==68);}
void udp_arm_peer(uint16_t p,uint32_t ip,uint16_t peer) {assert(!ip && peer==67);udp_arm(p);}
int net_send_udp(uint32_t s,uint16_t sp,uint32_t d,uint16_t dp,const void *p,uint32_t n) {
    assert(!s && sp==68 && d==UINT32_MAX && dp==67 && n<=sizeof request);
    memcpy(request,p,n);request_len=n;sends++;return 0;
}
int udp_collect(uint8_t *out,uint32_t cap,uint32_t *from) {
    (void)from;assert(cap>=300 && request_len>240);memcpy(out,request,240);out[0]=2;
    net_ip_wr(IPV4(192,168,5,24),out+16);unsigned n=240;uint8_t type=request[242]==1?2:5;
    out[n++]=53;out[n++]=1;out[n++]=type;
    out[n++]=54;out[n++]=4;net_ip_wr(IPV4(192,168,5,1),out+n);n+=4;
    out[n++]=1;out[n++]=4;net_ip_wr(IPV4(255,255,255,0),out+n);n+=4;
    out[n++]=3;out[n++]=4;net_ip_wr(IPV4(192,168,5,1),out+n);n+=4;
    out[n++]=6;out[n++]=4;net_ip_wr(IPV4(9,9,9,9),out+n);n+=4;
    out[n++]=51;out[n++]=4;net_ip_wr(120,out+n);n+=4;out[n++]=255;
    if(mode==1) out[240+4]=0;              /* short server-id option */
    if(mode==2) out[242]=0;                /* wrong DHCP message type */
    if(mode==3) out[28]^=1;                /* wrong client MAC */
    if(mode==4) out[236]^=1;               /* wrong magic */
    if(mode==5) out[2]=5;                  /* wrong hardware address length */
    if(mode==6) out[4]^=1;                 /* wrong transaction */
    if(mode==7) out[251]=0xfd;             /* noncontiguous subnet mask */
    if(mode==8) out[257]=7;                /* off-subnet router */
    if(mode==9 && type==5) out[19]=25;     /* ACK changes requested address */
    if(mode==10) out[n-1]=0;               /* missing END */
    if(mode==11 && type==5) out[248]=2;    /* ACK from different server */
    if(mode==12) n=242;                    /* truncated message type */
    if(mode==13) {g_netif.up=false;}       /* lost link while waiting */
    if(mode==14) { /* A server may legitimately supply no default route/DNS. */
        memmove(out+255,out+267,n-267);n-=12;
    }
    if(mode==15 && type==5) out[242]=6; /* NACK invalidates an existing lease */
    if(mode==16 && type==5) {out[242]=6;out[248]=2;} /* another server's NACK */
    return n;
}
static void reset(unsigned kind) {
    memset(&g_netif,0,sizeof g_netif);g_netif.up=true;memcpy(g_netif.mac,"\0\21\42\63\104\125",6);
    mode=kind;sends=arp_resets=0;ticks=0;net_dhcp_forget_lease();
}
int main(void) {
    reset(0);assert(net_dhcp());assert(sends==2 && arp_resets==1 && g_netif.dhcp);
    assert(g_netif.ip==IPV4(192,168,5,24) && g_netif.gateway==IPV4(192,168,5,1) && g_netif.dns==IPV4(9,9,9,9));
    assert(net_dhcp_renew_after_ms()==60000 && net_dhcp_lease_remaining_ms()==120000);
    ticks=11999;assert(net_dhcp_lease_remaining_ms()==10);ticks=12000;assert(!net_dhcp_lease_remaining_ms());
    for(unsigned kind=1;kind<=13;kind++) {
        reset(kind);assert(!net_dhcp());assert(!g_netif.dhcp && !g_netif.ip && sends<=6);
    }
    reset(14);assert(net_dhcp());assert(!g_netif.gateway && !g_netif.dns); /* no VM gateway invented */
    reset(0);assert(net_dhcp());mode=15;assert(!net_dhcp());assert(!g_netif.dhcp && !g_netif.ip && !net_dhcp_lease_remaining_ms());
    reset(0);assert(net_dhcp());mode=16;assert(!net_dhcp());assert(g_netif.dhcp && g_netif.ip && net_dhcp_lease_remaining_ms());
    puts("DHCP: address/mask/router/DNS, lease clock, hostile option lengths/identity/server/ACK, link loss and no invented gateway passed");
}
