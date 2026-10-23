#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/wifi/wpa2_crypto.h"
/* aes.c's optional kernel selftest logger is not used by these host checks. */
void kprintf(const char *fmt,...) {(void)fmt;}
static void unhex(const char *s,uint8_t *p,unsigned n) {
 for(unsigned i=0;i<n;i++) {unsigned v;assert(sscanf(s+2*i,"%2x",&v)==1);p[i]=v;}
}
int main(void) {
 uint8_t pmk[32],expected[48],ptk[48],sta[]={0,0x11,0x22,0x33,0x44,0x55};
 uint8_t ap[]={0xaa,0xbb,0xcc,0xdd,0xee,0xff},sn[32],an[32],mic[16];
 assert(wifi_wpa2_psk((const uint8_t *)"password",8,(const uint8_t *)"IEEE",4,pmk)==0);
 unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",expected,32);
 assert(!memcmp(pmk,expected,32));
 for(unsigned i=0;i<32;i++){sn[i]=i;an[i]=i+32;}
 wifi_wpa2_ptk(pmk,sta,ap,sn,an,ptk);
 unhex("3b641730c52e0ac2e714dcacc1388f3266dd8a0609a5f3edc30916cd459f3a4caa12000ab891af976f42fed66c3e1847",expected,48);
 assert(!memcmp(ptk,expected,48));
 wifi_wpa2_ptk(pmk,ap,sta,an,sn,expected);assert(!memcmp(ptk,expected,48));
 wifi_wpa2_mic(ptk,(const uint8_t *)"test eapol packet",17,mic);
 unhex("bfd825772d9d3a7080d68a06ef56ec6a",expected,16);assert(!memcmp(mic,expected,16));
 uint8_t kek[16],wrapped[24],plain[16];
 unhex("000102030405060708090a0b0c0d0e0f",kek,16);
 unhex("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5",wrapped,24);
 assert(wifi_wpa2_unwrap(kek,wrapped,24,plain,16)==16);
 unhex("00112233445566778899aabbccddeeff",expected,16);assert(!memcmp(plain,expected,16));
 wrapped[0]^=1;assert(wifi_wpa2_unwrap(kek,wrapped,24,plain,16)==-1);
 for(unsigned i=0;i<16;i++) assert(!plain[i]);
 uint8_t rsn[]={1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
 assert(wifi_rsn_ccmp_psk(rsn,sizeof rsn)==0);
 for(unsigned i=0;i<18;i++) assert(wifi_rsn_ccmp_psk(rsn,i)==-1);
 rsn[18]=64;assert(wifi_rsn_ccmp_psk(rsn,sizeof rsn)==-1);rsn[18]=0;
 rsn[5]=2;assert(wifi_rsn_ccmp_psk(rsn,sizeof rsn)==-1);rsn[5]=4;
 rsn[7]=255;assert(wifi_rsn_ccmp_psk(rsn,sizeof rsn)==-1);
 assert(wifi_wpa2_psk((const uint8_t *)"short",5,(const uint8_t *)"IEEE",4,pmk)==-1);
 puts("WPA2: independent PMK/PTK/MIC vectors, RFC3394 unwrap/integrity and bounded RSN parsing passed");
}
