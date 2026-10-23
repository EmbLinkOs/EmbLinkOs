#include "wpa2_crypto.h"
#include "crypto/sha1.h"
#include "crypto/aes.h"
#include "include/kstring.h"
static void wipe(void *p,unsigned n) {volatile uint8_t *v=p;while(n--) *v++=0;}
static void hmac_sha1(const uint8_t *key,unsigned kn,const uint8_t *msg,unsigned n,uint8_t out[20]) {
    uint8_t pad[64]={0},inner[20];struct sha1_ctx c;
    if(kn>64) {sha1(key,kn,pad);kn=20;} else memcpy(pad,key,kn);
    for(unsigned i=0;i<64;i++) pad[i]^=0x36;
    sha1_init(&c);sha1_update(&c,pad,64);sha1_update(&c,msg,n);sha1_final(&c,inner);
    for(unsigned i=0;i<64;i++) pad[i]^=0x36^0x5c;
    sha1_init(&c);sha1_update(&c,pad,64);sha1_update(&c,inner,20);sha1_final(&c,out);
    wipe(pad,sizeof pad);wipe(inner,sizeof inner);wipe(&c,sizeof c);
}
static int hex(unsigned c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
int wifi_wpa2_psk(const uint8_t *password,unsigned length,const uint8_t *ssid,unsigned sn,uint8_t pmk[32]) {
    if(!password || !ssid || !pmk || !sn || sn>32 || length<8 || length>64) return -1;
    if(length==64) {
        for(unsigned i=0;i<32;i++) {
            int a=hex(password[i*2]),b=hex(password[i*2+1]);
            if(a<0 || b<0) {wipe(pmk,32);return -1;}
            pmk[i]=(a<<4)|b;
        }
        return 0;
    }
    for(unsigned i=0;i<length;i++) if(password[i]<32 || password[i]>126) return -1;
    uint8_t salt[36],u[20],accum[20];memcpy(salt,ssid,sn);
    for(unsigned block=1;block<=2;block++) {
        salt[sn]=salt[sn+1]=salt[sn+2]=0;salt[sn+3]=block;
        hmac_sha1(password,length,salt,sn+4,u);memcpy(accum,u,20);
        for(unsigned round=1;round<4096;round++) {
            hmac_sha1(password,length,u,20,u);
            for(unsigned i=0;i<20;i++) accum[i]^=u[i];
        }
        unsigned take=block==1?20:12;memcpy(pmk+(block-1)*20,accum,take);
    }
    wipe(salt,sizeof salt);wipe(u,sizeof u);wipe(accum,sizeof accum);return 0;
}
void wifi_wpa2_ptk(const uint8_t pmk[32],const uint8_t sta[6],const uint8_t ap[6],
    const uint8_t snonce[32],const uint8_t anonce[32],uint8_t out[48]) {
    static const char label[]="Pairwise key expansion";
    uint8_t input[sizeof label+76+1],digest[20];
    memcpy(input,label,sizeof label);
    const uint8_t *a=memcmp(sta,ap,6)<0?sta:ap,*b=a==sta?ap:sta;
    memcpy(input+sizeof label,a,6);memcpy(input+sizeof label+6,b,6);
    a=memcmp(snonce,anonce,32)<0?snonce:anonce;b=a==snonce?anonce:snonce;
    memcpy(input+sizeof label+12,a,32);memcpy(input+sizeof label+44,b,32);
    for(unsigned i=0;i<3;i++) {
        input[sizeof input-1]=i;hmac_sha1(pmk,32,input,sizeof input,digest);
        memcpy(out+i*20,digest,i==2?8:20);
    }
    wipe(input,sizeof input);wipe(digest,sizeof digest);
}
void wifi_wpa2_mic(const uint8_t kck[16],const uint8_t *packet,unsigned n,uint8_t mic[16]) {
    uint8_t digest[20];hmac_sha1(kck,16,packet,n,digest);memcpy(mic,digest,16);wipe(digest,20);
}
int wifi_wpa2_unwrap(const uint8_t kek[16],const uint8_t *wrapped,unsigned length,uint8_t *out,unsigned cap) {
    if(!kek || !wrapped || !out || length<24 || length>264 || (length&7) || cap<length-8) return -1;
    struct aes128_ctx aes;uint8_t a[8],b[16];unsigned n=length/8-1;
    aes128_init(&aes,kek);memcpy(a,wrapped,8);memcpy(out,wrapped+8,length-8);
    for(int j=5;j>=0;j--) for(unsigned i=n;i;i--) {
        memcpy(b,a,8);uint64_t t=(uint64_t)n*j+i;
        for(unsigned k=0;k<8;k++) b[7-k]^=t>>(k*8);
        memcpy(b+8,out+(i-1)*8,8);aes128_decrypt_block(&aes,b,b);
        memcpy(a,b,8);memcpy(out+(i-1)*8,b+8,8);
    }
    unsigned diff=0;for(unsigned i=0;i<8;i++) diff|=a[i]^0xa6;
    wipe(&aes,sizeof aes);wipe(a,sizeof a);wipe(b,sizeof b);
    if(diff) {wipe(out,length-8);return -1;}return length-8;
}
static unsigned le16(const uint8_t *p) {return p[0]|((unsigned)p[1]<<8);}
static int selector(const uint8_t *p,unsigned type) {return !p[0] && p[1]==0x0f && p[2]==0xac && p[3]==type;}
int wifi_rsn_ccmp_psk(const uint8_t *p,unsigned n) {
    if(!p || n<18 || n>255 || le16(p)!=1 || !selector(p+2,4)) return -1;
    unsigned at=6;int pair=0,psk=0;
    for(unsigned list=0;list<2;list++) {
        if(n-at<2) return -1;
        unsigned count=le16(p+at);at+=2;
        if(!count || count>(n-at)/4) return -1;
        for(unsigned i=0;i<count;i++,at+=4) if(selector(p+at,list?2:4)) {if(list) psk=1;else pair=1;}
    }
    if(!pair || !psk) return -1;
    if(at==n) return 0;
    if(n-at<2 || (le16(p+at)&(1<<6))) return -1;
    at+=2;
    if(at==n) return 0;
    if(n-at<2) return -1;
    unsigned ids=le16(p+at);at+=2;
    if(ids>(n-at)/16) return -1;
    at+=ids*16;
    return at==n || (n-at==4 && selector(p+at,6))?0:-1;
}
