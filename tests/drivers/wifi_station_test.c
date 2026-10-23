#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/wifi/station.h"
#include "wifi_vectors.h"
void kprintf(const char *fmt,...) {(void)fmt;}
struct radio {
    struct wifi_station station;
    unsigned tunes, prepares, associations, transmits, delivered, up, down;
    int fail_tx, fail_prepare, fail_associate, port;
    uint8_t channel, tx[WIFI_MPDU_MAX], eth[1514];
    unsigned tx_len, eth_len;
};
static int tune(void *arg,uint8_t channel) {struct radio *r=arg;r->tunes++;r->channel=channel;return 0;}
static int prepare(void *arg,const struct wifi_bss *b) {
    struct radio *r=arg;r->prepares++;assert(b->channel==r->channel);return r->fail_prepare?-1:0;
}
static int associate(void *arg,const struct wifi_bss *b) {
    struct radio *r=arg;r->associations++;assert(b->aid==42);return r->fail_associate?-1:0;
}
static int tx(void *arg,const uint8_t *p,unsigned n) {
    struct radio *r=arg;r->transmits++;assert(n<=sizeof r->tx);memcpy(r->tx,p,n);r->tx_len=n;
    return r->fail_tx?-1:0;
}
static int random_bytes(void *arg,uint8_t *p,unsigned n) {(void)arg;for(unsigned i=0;i<n;i++) p[i]=i;return 0;}
static void ether(void *arg,const uint8_t *p,unsigned n) {
    struct radio *r=arg;r->delivered++;assert(n<=sizeof r->eth);memcpy(r->eth,p,n);r->eth_len=n;
}
static void link(void *arg,int up) {struct radio *r=arg;r->port=up;if(up) r->up++;else r->down++;}
static const struct wifi_radio_ops ops={tune,prepare,associate,tx,random_bytes,ether,link};
static const uint8_t channels[]={1,6,11};
static const uint8_t bcast[]={255,255,255,255,255,255};
static void start(struct radio *r) {
    memset(r,0,sizeof(*r));assert(!wifi_station_init(&r->station,&ops,r,vector_sta));
    assert(!wifi_station_join(&r->station,(const uint8_t *)"IEEE",4,vector_pmk,channels,sizeof channels,0));
    assert(r->station.state==WIFI_STA_SCAN && !r->port && r->channel==1);
}
static void header(uint8_t *p,unsigned subtype,const uint8_t *dst) {
    memset(p,0,24);p[0]=subtype;memcpy(p+4,dst,6);memcpy(p+10,vector_ap,6);memcpy(p+16,vector_ap,6);
}
static unsigned beacon(uint8_t *p) {
    header(p,0x80,bcast);memset(p+24,0,12);p[32]=100;p[34]=0x31;p[35]=4;
    unsigned at=36;memcpy(p+at,"\0\4IEEE",6);at+=6;
    static const uint8_t rates[]={1,8,0x82,0x84,0x8b,0x96,12,18,24,36,50,4,48,72,96,108,3,1,1};
    memcpy(p+at,rates,sizeof rates);at+=sizeof rates;
    memcpy(p+at,vector_rsn,sizeof vector_rsn);return at+sizeof vector_rsn;
}
static void receive(struct radio *r,const uint8_t *p,unsigned n,uint64_t now) {wifi_station_rx(&r->station,p,n,1,now);}
static void associate_station(struct radio *r) {
    uint8_t p[256];unsigned n=beacon(p);receive(r,p,n,1);
    assert(r->prepares==1 && r->transmits==1 && r->tx[0]==0xb0 && r->tx_len==30);
    assert(!memcmp(r->tx+4,vector_ap,6) && !memcmp(r->tx+10,vector_sta,6));
    assert(r->station.state==WIFI_STA_AUTH);
    header(p,0xb0,vector_sta);p[1]=8; /* legitimate retried authentication response */
    memset(p+24,0,6);p[26]=2;receive(r,p,30,2);
    assert(r->station.state==WIFI_STA_ASSOC && r->tx[0]==0 && !r->port);
    assert(!memcmp(r->tx+r->tx_len-sizeof vector_rsn,vector_rsn,sizeof vector_rsn));
    header(p,0x10,vector_sta);p[24]=0x31;p[25]=4;p[26]=p[27]=0;p[28]=42;p[29]=0xc0;
    memcpy(p+30,"\1\4\202\204\213\226",6);receive(r,p,36,3);
    assert(r->station.state==WIFI_STA_HANDSHAKE && r->associations==1 && !r->port);
}
static unsigned eapol(uint8_t *p,const uint8_t *e,unsigned n) {
    header(p,0x08,vector_sta);p[1]=2;
    memcpy(p+24,"\252\252\3\0\0\0\210\216",8);memcpy(p+32,e,n);return n+32;
}
static void connected(struct radio *r) {
    uint8_t p[WIFI_MPDU_MAX];unsigned n=eapol(p,vector_m1,sizeof vector_m1);receive(r,p,n,4);
    assert(r->tx_len==32+sizeof vector_m2 && !memcmp(r->tx+32,vector_m2,sizeof vector_m2));
    assert(!r->port);
    n=eapol(p,vector_m3,sizeof vector_m3);receive(r,p,n,5);
    assert(r->station.state==WIFI_STA_UP && r->port && r->up==1);
    assert(r->tx_len==32+sizeof vector_m4 && !memcmp(r->tx+32,vector_m4,sizeof vector_m4));
}
static void data_path(void) {
    struct radio r;uint8_t p[WIFI_MPDU_MAX],encrypted[WIFI_MPDU_MAX],plain[WIFI_MPDU_MAX],eth[64]={0};
    start(&r);assert(wifi_station_tx(&r.station,eth,sizeof eth)==-1);
    associate_station(&r);assert(wifi_station_tx(&r.station,eth,sizeof eth)==-1);
    /* Data cannot pass before authentication or without CCMP. */
    receive(&r,vector_plain,sizeof vector_plain,4);assert(!r.delivered);
    connected(&r);receive(&r,vector_plain,sizeof vector_plain,6);assert(!r.delivered);
    receive(&r,vector_ccmp,sizeof vector_ccmp,7);
    assert(r.delivered==1 && r.eth_len==sizeof vector_plain-18);
    assert(!memcmp(r.eth,vector_sta,6) && r.eth[12]==8 && r.eth[13]==0);
    assert(!memcmp(r.eth+14,vector_plain+32,sizeof vector_plain-32));
    receive(&r,vector_ccmp,sizeof vector_ccmp,8);assert(r.delivered==1); /* replay */
    memcpy(eth,vector_ap,6);memcpy(eth+6,vector_sta,6);eth[12]=8;
    for(unsigned i=14;i<sizeof eth;i++) eth[i]=i;
    assert(!wifi_station_tx(&r.station,eth,sizeof eth));assert(r.tx[1]==0x41);
    struct wifi_ccmp_key peer={0};assert(!wifi_ccmp_install(&peer,vector_ptk+32,0,0));
    int n=wifi_ccmp_open(&peer,r.tx,r.tx_len,plain,sizeof plain);
    assert(n==(int)sizeof eth+18 && !memcmp(plain+4,vector_ap,6));
    assert(!memcmp(plain+32,eth+14,sizeof eth-14));
    /* Authenticated group-key rekey must stay inside the pairwise cipher. */
    unsigned en=eapol(p,vector_group_m1,sizeof vector_group_m1);
    receive(&r,p,en,9);assert(!r.station.group[2].installed);
    peer.tx_pn=r.station.pair.rx_pn[16];
    n=wifi_ccmp_seal(&peer,p,en,encrypted,sizeof encrypted);assert(n>0);receive(&r,encrypted,n,10);
    assert(r.station.group[2].installed && r.station.group[2].rx_pn[0]==9);
    assert(r.tx[1]==0x41);n=wifi_ccmp_open(&peer,r.tx,r.tx_len,plain,sizeof plain);assert(n>0);
    assert(!memcmp(plain+32,vector_group_m2,sizeof vector_group_m2));
    r.station.group[1].rx_pn[7]=200;
    en=eapol(p,vector_group_same_gtk_m1,sizeof vector_group_same_gtk_m1);
    n=wifi_ccmp_seal(&peer,p,en,encrypted,sizeof encrypted);assert(n>0);receive(&r,encrypted,n,11);
    assert(!r.station.group[1].installed && r.station.group[3].rx_pn[7]==200);
    /* A valid beacon keeps an idle association alive; a different BSS cannot. */
    en=beacon(p);receive(&r,p,en,29000);wifi_station_tick(&r.station,31000);assert(r.port);
    wifi_station_tick(&r.station,59000);assert(!r.port && r.station.error==WIFI_ERR_TIMEOUT);
    wifi_station_disconnect(&r.station);assert(r.station.state==WIFI_STA_DOWN && !r.station.pair.installed);
    for(unsigned i=0;i<32;i++) assert(!r.station.pmk[i]);
}
static void failures(void) {
    struct radio r;uint8_t p[WIFI_MPDU_MAX],bad[WIFI_MPDU_MAX];unsigned n;
    start(&r);n=beacon(p);
    for(unsigned i=0;i<n;i++) {receive(&r,p,i,1);assert(r.station.state==WIFI_STA_SCAN);}
    memcpy(bad,p,n);bad[34]&=~16;receive(&r,bad,n,1);assert(!r.prepares); /* open BSS */
    memcpy(bad,p,n);bad[44]=255;receive(&r,bad,n,1);assert(!r.prepares); /* unsupported mandatory rate */
    wifi_station_rx(&r.station,p,n,6,1);assert(!r.prepares); /* stale channel */
    r.fail_prepare=1;receive(&r,p,n,1);assert(r.station.state==WIFI_STA_FAILED && !r.port);
    start(&r);for(unsigned i=1;i<=6;i++) wifi_station_tick(&r.station,i*250);
    assert(r.station.state==WIFI_STA_FAILED && r.station.error==WIFI_ERR_NOT_FOUND && r.tunes==6);
    start(&r);n=beacon(p);receive(&r,p,n,1);
    for(unsigned i=1;i<=3;i++) wifi_station_tick(&r.station,i*1000+1);
    assert(r.station.state==WIFI_STA_FAILED && r.transmits==3);
    start(&r);associate_station(&r);wifi_station_tick(&r.station,4003);
    assert(r.station.state==WIFI_STA_FAILED && !r.port);
    start(&r);associate_station(&r);connected(&r);
    header(p,0xc0,vector_sta);p[24]=3;p[25]=0;receive(&r,p,26,6);
    assert(r.station.state==WIFI_STA_FAILED && r.station.error==WIFI_ERR_DISCONNECTED && !r.port);
    unsigned seed=0x30458291;
    for(unsigned run=0;run<10000;run++) {
        start(&r);n=beacon(p);memset(p+n,0,sizeof p-n);
        seed=seed*1664525+1013904223;unsigned count=seed%(sizeof p+1);
        for(unsigned i=0;i<count;i++) {seed=seed*1664525+1013904223;if(!(seed&7)) p[i]^=seed>>24;}
        receive(&r,p,count,1);assert(!r.port && !r.delivered);
    }
}
int main(void) {
    data_path();failures();
    puts("Wi-Fi station: passive scan, auth/association, WPA2, controlled Ethernet/CCMP path, protected GTK rekey, retries/disconnects and 10k malformed management frames passed");
}
