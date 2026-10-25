#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "block/install.h"
#include "update/protocol.h"
#include "include/errno.h"
#define SECTORS 8192u
struct disk {unsigned char *bytes;unsigned writes;int fail,corrupt;};
static int rd(void *v,uint64_t l,uint32_t n,void *b) {
    struct disk *d=v;if(l>SECTORS || n>SECTORS-l)return -1;
    memcpy(b,d->bytes+l*512,n*512);if(d->corrupt && l==100) ((unsigned char *)b)[0]^=1;return 0;
}
static int wr(void *v,uint64_t l,uint32_t n,const void *b) {
    struct disk *d=v;d->writes++;if(d->fail && d->writes>=(unsigned)d->fail)return -1;
    if(l>SECTORS || n>SECTORS-l)return -1;
    memcpy(d->bytes+l*512,b,n*512);return 0;
}
static int flush(void *v) {(void)v;return 0;}
static void w32(unsigned char *b,uint32_t v) {memcpy(b,&v,4);}
static void w64(unsigned char *b,uint64_t v) {memcpy(b,&v,8);}
static uint64_t r64(const unsigned char *b) {uint64_t v;memcpy(&v,b,8);return v;}
static void seal(unsigned char *h) {w32(h+16,0);w32(h+16,crc32(0,h,92));}
static void fixture(struct disk *s) {
    memset(s->bytes,0,SECTORS*512);unsigned char *h=s->bytes+512,*e=s->bytes+1024;
    s->bytes[510]=0x55;s->bytes[511]=0xaa;s->bytes[450]=0xee;w32(s->bytes+454,1);w32(s->bytes+458,4095);
    const unsigned char esp[]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0,0xa0,0xc9,0x3e,0xc9,0x3b};
    const unsigned char data[]={0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4};
    memcpy(e,esp,16);e[16]=1;w64(e+32,2048);w64(e+40,2559);
    memcpy(e+128,data,16);e[144]=2;w64(e+160,2560);w64(e+168,3503);
    memcpy(h,"EFI PART",8);w32(h+8,0x10000);w32(h+12,92);w64(h+24,1);w64(h+32,4095);
    w64(h+40,34);w64(h+48,4062);h[56]=3;w64(h+72,2);w32(h+80,128);w32(h+84,128);
    w32(h+88,crc32(0,e,16384));seal(h);
    memcpy(s->bytes+4063*512,e,16384);unsigned char *backup=s->bytes+4095*512;memcpy(backup,h,512);
    w64(backup+24,4095);w64(backup+32,1);w64(backup+72,4063);seal(backup);
    for(unsigned i=34*512;i<3504*512;i++)s->bytes[i]=(unsigned char)(i*7+13);
    unsigned char *sb=s->bytes+(2560+128)*512;memset(sb,0,512);
    memcpy(sb,"EMBKFS17",8);w32(sb+8,2);w64(sb+40,4096);w64(sb+48,118);w64(sb+56,80);
    uint32_t sum=~0u;
    for(unsigned i=0;i<152;i++){sum^=sb[i];for(unsigned j=0;j<8;j++)sum=(sum>>1)^(0x82f63b78u&(0u-(sum&1)));}
    w64(sb+152,~sum);memcpy(s->bytes+3496*512,sb,512);
}
static void managed_fixture(struct disk *s) {
    fixture(s);unsigned char *h=s->bytes+512,*e=s->bytes+1024;
    memcpy(e+256,e+128,128);memcpy(e+384,e+128,128);
    e[272]=3;e[400]=4;
    w64(e+168,2815);w64(e+256+32,2816);w64(e+256+40,3071);
    w64(e+384+32,3072);w64(e+384+40,4015);
    memcpy(s->bytes+(3072+128)*512,s->bytes+(2560+128)*512,512);
    memcpy(s->bytes+(4016-8)*512,s->bytes+(2560+128)*512,512);
    unsigned char *esp=s->bytes+2048*512;memcpy(esp+3,"EMBUPD1 ",8);esp[14]=128;esp[15]=0;
    struct update_state state={0};state.revision=1;state.active=0;
    state.pending=state.previous=UPDATE_NO_SLOT;
    memcpy(state.esp_guid,e+16,16);memcpy(state.data_guid,e+384+16,16);
    memcpy(state.slot[0].root_guid,e+128+16,16);memcpy(state.slot[1].root_guid,e+256+16,16);
    state.slot[0].release=1;state.slot[0].root_bytes=256*512;state.slot[0].efi_bytes=512;
    strcpy(state.slot[0].version,"1.0.0");memset(state.slot[0].efi_sha256,1,32);memset(state.slot[0].root_sha256,2,32);
    assert(!update_state_encode(&state,esp+UPDATE_CONTROL0_OFFSET));
    assert(!update_state_encode(&state,esp+UPDATE_CONTROL1_OFFSET));
    assert(!update_state_encode(&state,esp+UPDATE_FACTORY_OFFSET));
    w32(h+88,crc32(0,e,16384));seal(h);memcpy(s->bytes+4063*512,e,16384);
    unsigned char *backup=s->bytes+4095*512;memcpy(backup,h,512);
    w64(backup+24,4095);w64(backup+32,1);w64(backup+72,4063);seal(backup);
}
int main(void) {
    struct disk s={calloc(SECTORS,512),0,0,0},d={calloc(SECTORS,512),0,0,0};assert(s.bytes&&d.bytes);
    struct install_io src={&s,4096,rd,wr,flush},dst={&d,8192,rd,wr,flush};
    struct install_plan *p=malloc(sizeof *p);void *b=malloc(65536),*v=malloc(65536);assert(p&&b&&v);
    unsigned char ids[52];for(unsigned i=0;i<52;i++)ids[i]=(unsigned char)(i+1);
    fixture(&s);assert(!install_prepare(&src,&dst,p));assert(d.writes==0 && p->copy_end==3504);
    assert(install_prepare(&src,&src,p)==-EMBK_EINVAL);
    struct install_io small=dst;small.sectors=4095;assert(install_prepare(&src,&small,p)==-EMBK_EINVAL);
    fixture(&s);struct install_io padded=src,middle=dst;padded.sectors=8192;middle.sectors=6000;
    assert(!install_prepare(&padded,&middle,p) && p->source_gpt_sectors==4096);
    fixture(&s);s.bytes[4095*512+16]^=1;assert(install_prepare(&src,&dst,p)<0);
    fixture(&s);s.bytes[1024+64]^=1;assert(install_prepare(&src,&dst,p)<0);
    fixture(&s);assert(!install_prepare(&src,&dst,p));s.bytes[1024]^=1;
    assert(install_copy(&src,&dst,p,ids,b,v,65536,NULL)<0 && !d.writes);
    fixture(&s);assert(!install_prepare(&src,&dst,p));d.fail=5;
    assert(install_copy(&src,&dst,p,ids,b,v,65536,NULL)==-EMBK_EIO);
    assert(d.bytes[510]==0 && !memcmp(d.bytes+512,"\0\0\0\0",4));
    d.fail=0;d.writes=0;assert(!install_prepare(&src,&dst,p));
    assert(!install_copy(&src,&dst,p,ids,b,v,65536,NULL));
    assert(!memcmp(s.bytes+34*512,d.bytes+34*512,(2688-34)*512));
    assert(!memcmp(s.bytes+2696*512,d.bytes+2696*512,(3504-2696)*512));
    uint64_t new_sectors=(8192-33-2560)/8*8;
    assert(r64(d.bytes+1024+168)==2560+new_sectors-1);
    assert(r64(d.bytes+2688*512+48)==new_sectors/8);
    assert(r64(d.bytes+2688*512+56)==80+new_sectors/8-118);
    assert(!memcmp(d.bytes+2688*512,d.bytes+(2560+new_sectors-8)*512,4096));
    unsigned char *h=d.bytes+512;uint32_t expected;memcpy(&expected,h+16,4);w32(h+16,0);
    assert(crc32(0,h,92)==expected);w32(h+16,expected);
    assert(r64(h+32)==8191 && r64(h+48)==8158 && !memcmp(h+56,ids,16));
    assert(r64(d.bytes+8191*512+72)==8159 && !memcmp(d.bytes+8159*512,d.bytes+1024,16384));
    assert(!memcmp(d.bytes+1040,ids+16,16) && !memcmp(d.bytes+1168,ids+32,16));
    assert(!memcmp(d.bytes+440,ids+48,4));
    managed_fixture(&s);d.writes=0;assert(!install_prepare(&src,&dst,p));assert(p->managed && p->root_start==3072);
    assert(!install_copy(&src,&dst,p,ids,b,v,65536,NULL));
    assert(!memcmp(s.bytes+2560*512,d.bytes+2560*512,512*512)); /* both OS slots unchanged */
    assert(r64(d.bytes+1024+128+40)==2815 && r64(d.bytes+1024+256+40)==3071);
    assert(r64(d.bytes+1024+384+40)>4015);
    struct update_state state;unsigned char *record=d.bytes+2048*512+UPDATE_CONTROL0_OFFSET;
    assert(!update_state_decode(record,512,&state));
    assert(!memcmp(state.esp_guid,d.bytes+1040,16) && !memcmp(state.data_guid,d.bytes+1024+384+16,16));
    assert(!memcmp(state.slot[0].root_guid,d.bytes+1024+128+16,16) && !memcmp(state.slot[1].root_guid,d.bytes+1024+256+16,16));
    assert(!memcmp(record,d.bytes+2048*512+UPDATE_CONTROL1_OFFSET,512) && !memcmp(record,d.bytes+2048*512+UPDATE_FACTORY_OFFSET,512));
    assert(memcmp(state.slot[0].root_guid,state.slot[1].root_guid,16));
    managed_fixture(&s);s.bytes[2048*512+UPDATE_CONTROL0_OFFSET]^=1;s.bytes[2048*512+UPDATE_CONTROL1_OFFSET]^=1;
    assert(install_prepare(&src,&dst,p)<0);
    free(s.bytes);free(d.bytes);free(p);free(b);free(v);
    puts("installer: GPT/backup bounds, source protection, stale plan, failed writes, verified legacy/managed clones, independent slot/data identities and relocated GPT OK");return 0;
}
