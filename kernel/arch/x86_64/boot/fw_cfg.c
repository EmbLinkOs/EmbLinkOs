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
/* A one-byte fw_cfg file the test harness passed with -fw_cfg, or -1. Only
 * under QEMU (TCG or KVM, with the QEMU fw_cfg signature). */
static int qemu_byte(const char *name,unsigned name_len) {
    unsigned a,b,c,d;LIVE_CPUID(1,&a,&b,&c,&d);if(!(c&(1u<<31)))return -1;
    LIVE_CPUID(0x40000000,&a,&b,&c,&d);
    char vendor[12];memcpy(vendor,&b,4);memcpy(vendor+4,&c,4);memcpy(vendor+8,&d,4);
    if(memcmp(vendor,"TCGTCGTCGTCG",12) && memcmp(vendor,"KVMKVMKVM\0\0\0",12))return -1;
    LIVE_SELECT(0);char signature[4];for(unsigned i=0;i<4;i++)signature[i]=LIVE_BYTE();
    if(memcmp(signature,"QEMU",4))return -1;
    LIVE_SELECT(0x19);unsigned count=0;
    for(unsigned i=0;i<4;i++)count=(count<<8)|LIVE_BYTE();
    if(count>128)return -1;
    for(unsigned i=0;i<count;i++) {
        uint8_t e[64];for(unsigned j=0;j<64;j++)e[j]=LIVE_BYTE();
        if(memcmp(e+8,name,name_len))continue;
        unsigned size=((unsigned)e[0]<<24)|((unsigned)e[1]<<16)|((unsigned)e[2]<<8)|e[3];
        unsigned selector=((unsigned)e[4]<<8)|e[5];
        if(size!=1 || selector<0x20 || selector>=0x4000)return -1;
        LIVE_SELECT(selector);return LIVE_BYTE();
    }
    return -1;
}
int boot_qemu_live(void) {
    static const char name[]="opt/emblink/live";
    return qemu_byte(name,sizeof name)=='1';
}
/* QEMU's virtio-gpu reports a second head as connected only while a UI window
 * shows it; under -display none it never does. A headless multi-monitor test
 * says how many heads to treat as connected with -fw_cfg
 * name=opt/emblink/outputs,string=N. */
int boot_qemu_outputs(void) {
    static const char name[]="opt/emblink/outputs";
    int v=qemu_byte(name,sizeof name);
    return v>='1' && v<='4'?v-'0':0;
}
