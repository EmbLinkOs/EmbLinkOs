#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/net.h"
struct netif g_netif;
static uint64_t ticks;
void net_lock(void) {}
void net_unlock(void) {}
uint64_t net_ticks(void) {return ticks;}
void net_wait(void) {ticks++;}
uint16_t net_l4_checksum(uint32_t a,uint32_t b,uint8_t c,const void *d,uint32_t e) {(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
int ip_output(uint32_t a,uint32_t b,uint8_t c,const void *d,uint32_t e) {(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
static void packet(uint32_t ip,unsigned sp,unsigned dp,const char *body,unsigned claimed_len) {
    uint8_t p[12];struct udp_hdr *h=(void *)p;
    h->src_port=htons(sp);h->dst_port=htons(dp);h->len=htons(claimed_len);h->checksum=0;
    memcpy(p+8,body,4);assert(udp_input(ip,p,sizeof p));
}
int main(void) {
    uint8_t out[16];uint32_t ip=0,server=IPV4(10,0,2,3);
    udp_arm_peer(49152,server,53);
    packet(server+1,53,49152,"BAD1",12);packet(server,53000,49152,"BAD2",12);
    packet(server,53,49152,"BAD3",7);packet(server,53,49152,"BAD4",13);
    packet(server,53,49152,"GOOD",12);
    assert(udp_collect(out,sizeof out,&ip)==4 && !memcmp(out,"GOOD",4) && ip==server);
    udp_arm_peer(68,0,67);packet(server,9000,68,"FAIL",12);packet(server+1,67,68,"PASS",12);
    assert(udp_collect(out,sizeof out,&ip)==4 && !memcmp(out,"PASS",4) && ip==server+1);
    puts("UDP capture: DHCP/DNS peer address/port filtering and malformed datagram lengths passed");
}
