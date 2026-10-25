#include "release.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) {uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static uint64_t u64(const uint8_t *p) {uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static int text(const uint8_t *p,unsigned n,int version) {
    unsigned i=0;for(;i<n && p[i];i++) {
        if(version) {if(!((p[i]>='A' && p[i]<='Z') || (p[i]>='a' && p[i]<='z') ||
            (p[i]>='0' && p[i]<='9') || p[i]=='.' || p[i]=='_' || p[i]=='-'))return 0;}
        else if(p[i]<32 || p[i]>126)return 0;
    }
    if(i==n || (version && !i))return 0;
    for(;i<n;i++)if(p[i])return 0;
    return 1;
}
int update_release_parse(const uint8_t *p,size_t n,struct embk_update_image *image,char notes[256]) {
    if(!p || !image || !notes || n!=RELEASE_BYTES || memcmp(p,"EMUREL1\0",8) ||
       u32(p+8)!=1 || u32(p+12)!=RELEASE_BYTES || u32(p+40)!=1 || u32(p+44)!=1 ||
       memcmp(p+48,"x86_64\0\0\0\0\0\0\0\0\0\0",16) || !text(p+64,32,1) || !text(p+160,224,0))return -1;
    uint64_t release=u64(p+16),root=u64(p+24),efi=u64(p+32);
    if(!release || !root || root>(8ULL<<30) || (root&511) || !efi || efi>(16ULL<<20))return -1;
    unsigned rh=0,eh=0;for(unsigned i=0;i<32;i++){rh|=p[96+i];eh|=p[128+i];}if(!rh || !eh)return -1;
    memset(image,0,sizeof *image);image->release=release;image->root_bytes=root;image->efi_bytes=efi;
    memcpy(image->version,p+64,32);memcpy(image->root_sha256,p+96,32);memcpy(image->efi_sha256,p+128,32);
    memset(notes,0,256);memcpy(notes,p+160,224);return 0;
}
