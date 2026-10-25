#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static unsigned hv=1,bad_vendor,selector,index,io_count,file_count=1;
static uint8_t directory[68],value='1';
static void cpuid(unsigned l,unsigned *a,unsigned *b,unsigned *c,unsigned *d){
    *a=*b=*c=*d=0;
    if(l==1)*c=hv?1u<<31:0;
    else {const char *s=bad_vendor?"OtherOther12":"TCGTCGTCGTCG";memcpy(b,s,4);memcpy(c,s+4,4);memcpy(d,s+8,4);}
}
static void select_model(unsigned n){selector=n;index=0;io_count++;}
static uint8_t byte_model(void){io_count++;if(!selector)return (uint8_t)"QEMU"[index++%4];if(selector==0x19){if(index<4){unsigned i=index++;return file_count>>(24-i*8);}return directory[index++-4];}assert(selector==0x20);return value;}
#define LIVE_CPUID cpuid
#define LIVE_SELECT select_model
#define LIVE_BYTE byte_model
#include "arch/x86_64/boot/fw_cfg.c"
int main(void){
    directory[3]=1;directory[5]=0x20;memcpy(directory+8,"opt/emblink/live",17);
    assert(boot_qemu_live());value='0';assert(!boot_qemu_live());
    hv=0;io_count=0;assert(!boot_qemu_live()&&!io_count);hv=1;bad_vendor=1;assert(!boot_qemu_live()&&!io_count);
    bad_vendor=0;file_count=129;assert(!boot_qemu_live());
    puts("live boot: explicit QEMU file option, no default bypass, bounded directory and no physical I/O probing OK");
}
