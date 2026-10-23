/* An explicit live-session option for disposable QEMU UI tests. Normal boots
 * authenticate. Never probe these legacy I/O ports on a physical machine. */
#include "arch/x86_64/boot/fw_cfg.h"
#include "include/kstring.h"
#include "include/io.h"
#ifndef LIVE_CPUID
static void live_cpuid(unsigned leaf,unsigned *a,unsigned *b,unsigned *c,unsigned *d) {
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(0));
}
#define LIVE_CPUID live_cpuid
#define LIVE_SELECT(v) outw(0x510,(v))
#define LIVE_BYTE() inb(0x511)
#endif
int boot_qemu_live(void) {
    unsigned a,b,c,d;LIVE_CPUID(1,&a,&b,&c,&d);if(!(c&(1u<<31)))return 0;
    LIVE_CPUID(0x40000000,&a,&b,&c,&d);
    char vendor[12];memcpy(vendor,&b,4);memcpy(vendor+4,&c,4);memcpy(vendor+8,&d,4);
    if(memcmp(vendor,"TCGTCGTCGTCG",12) && memcmp(vendor,"KVMKVMKVM\0\0\0",12))return 0;
    LIVE_SELECT(0);char signature[4];for(unsigned i=0;i<4;i++)signature[i]=LIVE_BYTE();
    if(memcmp(signature,"QEMU",4))return 0;
    LIVE_SELECT(0x19);unsigned count=0;
    for(unsigned i=0;i<4;i++)count=(count<<8)|LIVE_BYTE();
    if(count>128)return 0;
    for(unsigned i=0;i<count;i++) {
        uint8_t e[64];for(unsigned j=0;j<64;j++)e[j]=LIVE_BYTE();
        static const char name[]="opt/emblink/live";
        if(memcmp(e+8,name,sizeof name))continue;
        unsigned size=((unsigned)e[0]<<24)|((unsigned)e[1]<<16)|((unsigned)e[2]<<8)|e[3];
        unsigned selector=((unsigned)e[4]<<8)|e[5];
        if(size!=1 || selector<0x20 || selector>=0x4000)return 0;
        LIVE_SELECT(selector);return LIVE_BYTE()=='1';
    }
    return 0;
}
