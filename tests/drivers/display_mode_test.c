#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../boot/uefi/display_mode.h"
int main(void){
    assert(efi_mode_usable(1920,1080,1920,1));assert(!efi_mode_usable(1920,1080,1919,1));
    assert(!efi_mode_usable(3841,2160,3841,1));assert(!efi_mode_usable(800,600,800,2));
    uint8_t e[128]={0,255,255,255,255,255,255,0};
    e[18]=1;e[19]=4;e[24]=2;e[54]=1;e[56]=128;e[58]=0x70;e[59]=56;e[61]=0x40;
    unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=e[i];e[127]=(uint8_t)-sum;
    unsigned w=0,h=0;assert(edid_preferred(e,128,&w,&h)&&w==1920&&h==1080);
    e[127]^=1;assert(!edid_preferred(e,128,&w,&h));assert(!edid_preferred(e,127,&w,&h));
    puts("UEFI display: bounded linear RGB modes, independently encoded preferred timing and invalid EDID refusal OK");
}
