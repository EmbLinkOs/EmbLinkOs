#ifndef EMBLINK_EFI_DISPLAY_MODE_H
#define EMBLINK_EFI_DISPLAY_MODE_H
#include <stdint.h>
/* Bounded GOP/EDID validation also compiled by the host tests. */
static inline int efi_mode_usable(uint32_t w,uint32_t h,uint32_t pitch,unsigned format) {
    return w>=320 && h>=200 && w<=3840 && h<=2160 && pitch>=w && pitch<=16384 &&
        format<=1 && (uint64_t)pitch*h*4<=64u*1024u*1024u;
}
static inline int edid_preferred(const uint8_t *p,unsigned n,unsigned *w,unsigned *h) {
    static const uint8_t header[8]={0,255,255,255,255,255,255,0};
    if(!p || n<128)return 0;
    for(unsigned i=0;i<8;i++)if(p[i]!=header[i])return 0;
    unsigned sum=0;for(unsigned i=0;i<128;i++)sum+=p[i];
    if((sum&255) || p[18]!=1 || p[19]>4 || !(p[24]&2) || !(p[54]|p[55]))return 0;
    unsigned x=p[56]|((p[58]&0xf0u)<<4),y=p[59]|((p[61]&0xf0u)<<4);
    if(!efi_mode_usable(x,y,x,1))return 0;
    *w=x;*h=y;return 1;
}
#endif
