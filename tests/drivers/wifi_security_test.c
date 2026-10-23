#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/wifi/wpa2.h"
#include "net/wifi/ccmp.h"
#include "wifi_vectors.h"
void kprintf(const char *fmt,...) {(void)fmt;}
struct fixture {
    struct wifi_wpa2 s;
    struct wifi_ccmp_key pair, group[4];
    unsigned sends, installs, port_up, port_down, random_calls;
    int tx_fail, rng_fail, install_fail, port, protected_frame;
    uint8_t sent[WIFI_EAPOL_MAX];
    unsigned sent_len;
};
static int random_bytes(void *arg,uint8_t *out,unsigned n) {
    struct fixture *f=arg;f->random_calls++;
    for(unsigned i=0;i<n;i++) out[i]=i;
    return f->rng_fail?-1:0;
}
static int send_frame(void *arg,const uint8_t *p,unsigned n,int prot) {
    struct fixture *f=arg;assert(n<=sizeof f->sent);f->sends++;
    memcpy(f->sent,p,n);f->sent_len=n;f->protected_frame=prot;return f->tx_fail?-1:0;
}
static int install(void *arg,const struct wifi_wpa2_keys *keys) {
    struct fixture *f=arg;f->installs++;
    if(f->install_fail) return -1;
    if(keys->pairwise) assert(!wifi_ccmp_install(&f->pair,keys->tk,0,0));
    if(keys->group) assert(!wifi_ccmp_install(&f->group[keys->gtk_id],keys->gtk,keys->gtk_id,keys->group_rsc));
    return 0;
}
static void port(void *arg,int up) {
    struct fixture *f=arg;f->port=up;if(up) f->port_up++;else f->port_down++;
}
static const struct wifi_wpa2_ops ops={random_bytes,send_frame,install,port};
static void start(struct fixture *f) {
    memset(f,0,sizeof(*f));
    assert(!wifi_wpa2_start(&f->s,&ops,f,vector_sta,vector_ap,vector_pmk,vector_rsn,sizeof vector_rsn,0));
    assert(f->s.state==WIFI_WPA2_M1 && !f->port);
}
static void rx(struct fixture *f,const uint8_t *p,unsigned n) {assert(wifi_wpa2_rx(&f->s,p,n,10)==1);}
#define RX(f,v) rx(f,vector_##v,sizeof vector_##v)
#define SENT(f,v) do {assert((f)->sent_len==sizeof vector_##v);assert(!memcmp((f)->sent,vector_##v,sizeof vector_##v));} while(0)
static void handshake(void) {
    struct fixture f;start(&f);
    assert(!wifi_wpa2_rx(&f.s,vector_m3,sizeof vector_m3,0));
    RX(&f,m1);SENT(&f,m2);assert(f.random_calls==1 && f.installs==0 && !f.port);
    assert(!memcmp(f.s.ptk,vector_ptk,48));
    RX(&f,m1);SENT(&f,m2);assert(f.random_calls==1); /* identical SNonce */
    RX(&f,m3);SENT(&f,m4);assert(f.installs==1 && f.port && f.port_up==1);
    assert(!memcmp(f.pair.tk,vector_ptk+32,16));
    assert(!memcmp(f.group[1].tk,vector_gtk,16));assert(f.group[1].rx_pn[16]==7);
    f.pair.tx_pn=100;f.pair.rx_pn[16]=80;f.group[1].rx_pn[16]=90;
    RX(&f,m3);SENT(&f,m4); /* KRACK: never reinstall or reset PN */
    assert(f.installs==1 && f.pair.tx_pn==100 && f.pair.rx_pn[16]==80 && f.group[1].rx_pn[16]==90);
    assert(!wifi_wpa2_rx(&f.s,vector_m1,sizeof vector_m1,11));
    RX(&f,group_m1);SENT(&f,group_m2);assert(f.installs==2 && f.protected_frame);
    f.group[2].rx_pn[3]=200;
    RX(&f,group_m1);assert(f.installs==2 && f.group[2].rx_pn[3]==200);
    assert(!wifi_wpa2_rx(&f.s,vector_m3,sizeof vector_m3,12));
    wifi_wpa2_stop(&f.s);assert(!f.port && f.s.state==WIFI_WPA2_IDLE);
    for(unsigned i=0;i<sizeof f.s;i++) assert(!((uint8_t *)&f.s)[i]);
}
static void failures(void) {
    struct fixture f;uint8_t packet[WIFI_EAPOL_MAX];
    start(&f);f.rng_fail=1;assert(wifi_wpa2_rx(&f.s,vector_m1,sizeof vector_m1,0)==-1);
    assert(f.s.state==WIFI_WPA2_FAILED && !f.port && !f.sends);
    start(&f);wifi_wpa2_tick(&f.s,4000);assert(f.s.state==WIFI_WPA2_FAILED);
    start(&f);RX(&f,m1);
    wifi_wpa2_tick(&f.s,1010);wifi_wpa2_tick(&f.s,2010);wifi_wpa2_tick(&f.s,3010);
    assert(f.sends==4);wifi_wpa2_tick(&f.s,4010);assert(f.s.state==WIFI_WPA2_FAILED);
    start(&f);RX(&f,m1);
    for(unsigned i=0;i<sizeof vector_m3;i++) {
        memcpy(packet,vector_m3,sizeof vector_m3);packet[i]^=0x80;
        assert(wifi_wpa2_rx(&f.s,packet,sizeof vector_m3,15)==0);
        assert(!f.installs && !f.port);
    }
    for(unsigned i=0;i<sizeof vector_m3;i++) assert(!wifi_wpa2_rx(&f.s,vector_m3,i,15));
    assert(!wifi_wpa2_rx(&f.s,vector_bad_rsn_m3,sizeof vector_bad_rsn_m3,15));
    assert(!wifi_wpa2_rx(&f.s,vector_no_gtk_m3,sizeof vector_no_gtk_m3,15));
    assert(!f.installs && !f.port);
    f.tx_fail=1;RX(&f,m3);assert(f.s.state==WIFI_WPA2_M4 && !f.port && f.installs==1);
    f.pair.tx_pn=42;f.tx_fail=0;wifi_wpa2_tick(&f.s,1010);
    assert(f.s.state==WIFI_WPA2_CONNECTED && f.port && f.installs==1 && f.pair.tx_pn==42);
    start(&f);RX(&f,m1);f.install_fail=1;
    assert(wifi_wpa2_rx(&f.s,vector_m3,sizeof vector_m3,10)==-1);assert(!f.port);
    /* Mutation corpus exercises authenticated-length parsing too. Most frames
     * fail MIC; the separately signed malformed KDE vectors cover that layer. */
    unsigned seed=0x72390142;
    for(unsigned run=0;run<10000;run++) {
        start(&f);RX(&f,m1);memcpy(packet,vector_m3,sizeof vector_m3);
        seed=seed*1664525+1013904223;unsigned n=seed%(sizeof packet+1);
        for(unsigned i=0;i<sizeof packet;i++) {
            seed=seed*1664525+1013904223;
            if(i>=sizeof vector_m3 || !(seed&7)) packet[i]=seed>>24;
        }
        (void)wifi_wpa2_rx(&f.s,packet,n,run);
    }
}
static void ccmp(void) {
    struct wifi_ccmp_key key={0},rxkey={0};uint8_t encrypted[WIFI_MPDU_MAX],out[WIFI_MPDU_MAX],bad[WIFI_MPDU_MAX];
    assert(!wifi_ccmp_install(&key,vector_ptk+32,0,0));
    assert(!wifi_ccmp_install(&rxkey,vector_ptk+32,0,0));
    key.tx_pn=UINT64_C(0x060504030200);
    int n=wifi_ccmp_seal(&key,vector_plain,sizeof vector_plain,encrypted,sizeof encrypted);
    assert(n==(int)sizeof vector_ccmp && !memcmp(encrypted,vector_ccmp,n));
    memset(out,0xa5,sizeof out);
    /* Every ciphertext/tag mutation fails without exposing any plaintext or
     * consuming the RX PN. Header changes covered by AAD also fail. */
    for(unsigned i=4;i<sizeof vector_ccmp;i++) {
        if(i==22 || i==23) continue; /* masked sequence number */
        memcpy(bad,vector_ccmp,sizeof vector_ccmp);bad[i]^=0x80;
        assert(wifi_ccmp_open(&rxkey,bad,sizeof vector_ccmp,out,sizeof out)==-1);
        assert(!rxkey.rx_pn[16]);
        for(unsigned j=0;j<sizeof out;j++) assert(out[j]==0xa5);
    }
    assert(wifi_ccmp_open(&rxkey,encrypted,n,out,sizeof out)==(int)sizeof vector_plain);
    assert(!memcmp(out,vector_plain,sizeof vector_plain));
    assert(wifi_ccmp_open(&rxkey,encrypted,n,out,sizeof out)==-1);
    /* Identical-key reinstall cannot undo TX/RX replay protection. */
    uint64_t tx=key.tx_pn,rx=rxkey.rx_pn[16];
    assert(!wifi_ccmp_install(&key,vector_ptk+32,0,0));assert(key.tx_pn==tx);
    assert(!wifi_ccmp_install(&rxkey,vector_ptk+32,0,0));assert(rxkey.rx_pn[16]==rx);
    assert(wifi_ccmp_open(&rxkey,encrypted,n,out,sizeof out)==-1);
    key.tx_pn=UINT64_C(0x060504030200);
    n=wifi_ccmp_seal(&key,vector_plain_qos,sizeof vector_plain_qos,encrypted,sizeof encrypted);
    assert(n==(int)sizeof vector_ccmp_qos && !memcmp(encrypted,vector_ccmp_qos,n));
    assert(wifi_ccmp_open(&rxkey,encrypted,n,out,sizeof out)==(int)sizeof vector_plain_qos);
    assert(!memcmp(out,vector_plain_qos,sizeof vector_plain_qos));
    assert(rxkey.rx_pn[5]==rxkey.rx_pn[16]); /* distinct replay queues */
    key.tx_pn=WIFI_CCMP_PN_MAX;assert(wifi_ccmp_seal(&key,vector_plain,sizeof vector_plain,encrypted,sizeof encrypted)==-1);
    wifi_ccmp_clear(&key);assert(!key.installed);
    assert(wifi_ccmp_seal(&key,vector_plain,sizeof vector_plain,encrypted,sizeof encrypted)==-1);
}
int main(void) {
    handshake();failures();ccmp();
    puts("Wi-Fi security: independent four-way/group rekey and CCMP vectors, KRACK/replay, TX/install failures, timeouts and 10k malformed EAPOL frames passed");
}
