#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "net/wifi/profile.h"
#include "net/wifi/ax200_protocol.h"
#include "wifi_vectors.h"
static unsigned records;
static int record(void *arg,const uint8_t *p,unsigned n) {(void)arg;assert(n>=4);assert(p[0]==1);records++;return 0;}
static int refuse(void *arg,const uint8_t *p,unsigned n) {(void)arg;(void)p;(void)n;return -1;}
int main(void) {
    struct wifi_profile p;uint8_t text[149];
    const char *valid="ssid_hex=49454545\npsk_hex=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n";
    unsigned n=strlen(valid);assert(!wifi_profile_parse(&p,(const uint8_t *)valid,n));
    assert(p.ssid_len==4 && !memcmp(p.ssid,"IEEE",4) && !memcmp(p.pmk,vector_pmk,32));
    for(unsigned i=0;i<n;i++) assert(wifi_profile_parse(&p,(const uint8_t *)valid,i)==-1);
    memcpy(text,valid,n);text[n]=0;assert(wifi_profile_parse(&p,text,n+1)==-1);
    for(unsigned i=0;i<n;i++) {
        memcpy(text,valid,n);text[i]=0xff;assert(wifi_profile_parse(&p,text,n)==-1);
        for(unsigned j=0;j<sizeof p;j++) assert(!((uint8_t *)&p)[j]);
    }
    char long_ssid[148];memcpy(long_ssid,"ssid_hex=",9);memset(long_ssid+9,'a',64);
    memcpy(long_ssid+73,valid+17,74);assert(!wifi_profile_parse(&p,(const uint8_t *)long_ssid,147));assert(p.ssid_len==32);
    char short_ssid[85];memcpy(short_ssid,"ssid_hex=00",11);memcpy(short_ssid+11,valid+17,74);
    assert(!wifi_profile_parse(&p,(const uint8_t *)short_ssid,85) && p.ssid_len==1 && !p.ssid[0]);
    static uint8_t blob[AX200_FW_MAX],mutant[AX200_FW_MAX];static struct ax200_fw fw;
    FILE *file=fopen("system/firmware/intel/iwlwifi-cc-a0-77.ucode","rb");assert(file);
    unsigned size=fread(blob,1,sizeof blob,file);assert(size==1368100);assert(!fclose(file));
    assert(!ax200_fw_parse(&fw,blob,size));assert(fw.count[0]==14 && fw.count[1]==15 && fw.count[2]==19);
    assert(ax200_fw_command_version(&fw,1,8,0)==4 && ax200_fw_command_version(&fw,1,0x0d,0)==15);
    for(unsigned i=0;i<88;i++) assert(ax200_fw_parse(&fw,blob,i)==-1);
    unsigned at=88;
    while(at<size) {
        uint32_t length;memcpy(&length,blob+at+4,4);
        memcpy(mutant,blob,size);memset(mutant+at+4,255,4);assert(ax200_fw_parse(&fw,mutant,size)==-1);
        assert(ax200_fw_parse(&fw,blob,at+7)==-1);
        at+=8+((length+3)&~3U);
    }
    uint8_t buf[4096]={0};buf[0]=4;buf[4]=1;
    assert(!ax200_rx_records(buf,sizeof buf,record,0) && records==1);
    assert(ax200_rx_records(buf,sizeof buf,refuse,0)==-1);
    buf[0]=3;assert(ax200_rx_records(buf,sizeof buf,record,0)==-1);
    memset(buf,0,sizeof buf);buf[2]=0x55;buf[3]=0x55;assert(!ax200_rx_records(buf,sizeof buf,record,0));
    unsigned seed=0x75432091;
    for(unsigned run=0;run<10000;run++) {
        for(unsigned i=0;i<sizeof buf;i++) {seed=seed*1664525+1013904223;buf[i]=seed>>24;}
        seed=seed*1664525+1013904223;(void)ax200_rx_records(buf,seed%4097,refuse,0);
    }
    puts("Wi-Fi inputs: private profile edge lengths, pinned API-77 firmware TLV bounds and 10k malformed DMA records passed");
}
