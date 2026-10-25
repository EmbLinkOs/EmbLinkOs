/* IEEE 802.11 CCMP-128: AES-CCM with a 13-byte nonce and 8-byte tag. */
#include "ccmp.h"
#include "crypto/aes.h"
#include "include/kstring.h"
static void erase(void *p,unsigned n) {volatile uint8_t *v=p;while(n--) *v++=0;}
void wifi_ccmp_clear(struct wifi_ccmp_key *k) {if(k) erase(k,sizeof(*k));}
int wifi_ccmp_install(struct wifi_ccmp_key *k,const uint8_t tk[16],unsigned id,uint64_t rsc) {
    if(!k || !tk || id>3 || rsc>WIFI_CCMP_PN_MAX) return -1;
    if(k->installed && k->id==id && !memcmp(k->tk,tk,16)) {
        for(unsigned i=0;i<17;i++) if(k->rx_pn[i]<rsc) k->rx_pn[i]=rsc;
        return 0;
    }
    wifi_ccmp_clear(k);memcpy(k->tk,tk,16);k->id=id;k->installed=1;
    for(unsigned i=0;i<17;i++) k->rx_pn[i]=rsc;
    return 0;
}
static int header(const uint8_t *p,unsigned n,unsigned *tid) {
    if(n<24 || (p[0]!=0x08 && p[0]!=0x88) || (p[1]&3)==3 || !(p[1]&3) ||
       (p[1]&(0x80|4)) || (p[22]&15)) return -1;
    *tid=16;
    if(p[0]&0x80) {
        if(n<26 || (p[24]&0x80) || p[25]) return -1; /* A-MSDU/mesh */
        *tid=p[24]&15;return 26;
    }
    return 24;
}
static void parameters(const uint8_t *h,unsigned hl,unsigned tid,uint64_t pn,
    uint8_t nonce[13],uint8_t aad[24],unsigned *an) {
    nonce[0]=tid==16?0:tid;memcpy(nonce+1,h+10,6);
    for(unsigned i=0;i<6;i++) nonce[7+i]=pn>>(8*(5-i));
    aad[0]=h[0]&0x8f;aad[1]=h[1]&0xc7;memcpy(aad+2,h+4,18);
    aad[20]=h[22]&15;aad[21]=0;*an=22;
    if(hl==26) {aad[22]=tid;aad[23]=0;*an=24;}
}
static void mac_block(const struct aes128_ctx *aes,uint8_t state[16],const uint8_t b[16]) {
    for(unsigned i=0;i<16;i++) state[i]^=b[i];
    aes128_encrypt_block(aes,state,state);
}
static void ccm_mac(const struct aes128_ctx *aes,const uint8_t nonce[13],
    const uint8_t *aad,unsigned an,const uint8_t *p,unsigned n,uint8_t tag[16]) {
    uint8_t b[32]={0};b[0]=0x59;memcpy(b+1,nonce,13);b[14]=n>>8;b[15]=n;
    memset(tag,0,16);mac_block(aes,tag,b);memset(b,0,sizeof b);
    b[0]=an>>8;b[1]=an;memcpy(b+2,aad,an);
    mac_block(aes,tag,b);mac_block(aes,tag,b+16);
    for(unsigned at=0;at<n;at+=16) {
        unsigned take=n-at<16?n-at:16;memset(b,0,16);memcpy(b,p+at,take);mac_block(aes,tag,b);
    }
    erase(b,sizeof b);
}
static void ctr_block(const struct aes128_ctx *aes,const uint8_t nonce[13],unsigned i,uint8_t b[16]) {
    b[0]=1;memcpy(b+1,nonce,13);b[14]=i>>8;b[15]=i;aes128_encrypt_block(aes,b,b);
}
static void crypt(const struct aes128_ctx *aes,const uint8_t nonce[13],
    const uint8_t *src,unsigned n,uint8_t *dst) {
    uint8_t b[16];
    for(unsigned at=0;at<n;at+=16) {
        ctr_block(aes,nonce,at/16+1,b);unsigned take=n-at<16?n-at:16;
        for(unsigned i=0;i<take;i++) dst[at+i]=src[at+i]^b[i];
    }
    erase(b,sizeof b);
}
int wifi_ccmp_seal(struct wifi_ccmp_key *k,const uint8_t *in,unsigned n,uint8_t *out,unsigned cap) {
    unsigned tid;int hl;
    if(!k || !k->installed || !in || !out || in==out || n>WIFI_MPDU_MAX-16 ||
       cap<n+16 || (hl=header(in,n,&tid))<0 || (in[1]&0x40) || k->tx_pn==WIFI_CCMP_PN_MAX) return -1;
    uint64_t pn=++k->tx_pn;memcpy(out,in,hl);out[1]|=0x40;
    uint8_t *iv=out+hl;iv[0]=pn;iv[1]=pn>>8;iv[2]=0;iv[3]=0x20|(k->id<<6);
    for(unsigned i=0;i<4;i++) iv[4+i]=pn>>(16+i*8);
    uint8_t nonce[13],aad[24],tag[16],s0[16];unsigned an;
    struct aes128_ctx aes;aes128_init(&aes,k->tk);
    parameters(out,hl,tid,pn,nonce,aad,&an);ccm_mac(&aes,nonce,aad,an,in+hl,n-hl,tag);
    crypt(&aes,nonce,in+hl,n-hl,out+hl+8);ctr_block(&aes,nonce,0,s0);
    for(unsigned i=0;i<8;i++) out[n+8+i]=tag[i]^s0[i];
    erase(&aes,sizeof aes);erase(tag,sizeof tag);erase(s0,sizeof s0);return n+16;
}
int wifi_ccmp_open(struct wifi_ccmp_key *k,const uint8_t *in,unsigned n,uint8_t *out,unsigned cap) {
    unsigned tid;int hl;
    if(!k || !k->installed || !in || !out || in==out || n>WIFI_MPDU_MAX ||
       (hl=header(in,n,&tid))<0 || n<(unsigned)hl+16 || cap<n-16 || !(in[1]&0x40)) return -1;
    const uint8_t *iv=in+hl;
    if(iv[2] || iv[3]!=(uint8_t)(0x20|(k->id<<6))) return -1;
    uint64_t pn=iv[0]|((uint64_t)iv[1]<<8);
    for(unsigned i=0;i<4;i++) pn|=(uint64_t)iv[4+i]<<(16+i*8);
    if(!pn || pn<=k->rx_pn[tid]) return -1;
    /* Authentication uses a private scratch buffer; callers never see
     * decrypted bytes from an invalid tag, even briefly. */
    uint8_t plain[WIFI_MPDU_MAX],nonce[13],aad[24],tag[16],s0[16];unsigned an,body=n-hl-16;
    struct aes128_ctx aes;aes128_init(&aes,k->tk);
    parameters(in,hl,tid,pn,nonce,aad,&an);crypt(&aes,nonce,in+hl+8,body,plain);
    ccm_mac(&aes,nonce,aad,an,plain,body,tag);ctr_block(&aes,nonce,0,s0);
    unsigned diff=0;for(unsigned i=0;i<8;i++) diff|=tag[i]^s0[i]^in[n-8+i];
    if(!diff) {
        memcpy(out,in,hl);out[1]&=~0x40;memcpy(out+hl,plain,body);k->rx_pn[tid]=pn;
    }
    erase(plain,sizeof plain);erase(&aes,sizeof aes);erase(tag,sizeof tag);erase(s0,sizeof s0);
    return diff?-1:(int)(n-16);
}
