#include "hw_random.h"
static void cpuid(unsigned leaf,unsigned sub,unsigned *a,unsigned *b,unsigned *c,unsigned *d) {
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(sub));
}
static int word(uint64_t *v,int seed) {
    unsigned char ok;
    if(seed) __asm__ volatile("rdseed %0; setc %1":"=r"(*v),"=qm"(ok)::"cc");
    else __asm__ volatile("rdrand %0; setc %1":"=r"(*v),"=qm"(ok)::"cc");
    /* Several firmware/CPU defects have returned all-ones with CF set.
     * Reject stuck outputs, including all-zero, instead of using such nonces. */
    return ok && *v && *v!=UINT64_MAX;
}
int crypto_hw_random(uint8_t *p,unsigned n) {
    if(!p || n>4096) return -1;
    unsigned a,b,c,d,max,seed=0,random=0;cpuid(0,0,&max,&b,&c,&d);
    if(max>=1) {cpuid(1,0,&a,&b,&c,&d);random=(c>>30)&1;}
    if(max>=7) {cpuid(7,0,&a,&b,&c,&d);seed=(b>>18)&1;}
    uint64_t previous=0;unsigned at=0;
    while(at<n) {
        uint64_t value=0;int ok=0;
        if(seed) for(unsigned i=0;i<1024;i++) {
            if(word(&value,1) && value!=previous) {ok=1;break;}
            __asm__ volatile("pause");
        }
        if(!ok && random) for(unsigned i=0;i<32;i++) {
            if(word(&value,0) && value!=previous) {ok=1;break;}
            __asm__ volatile("pause");
        }
        if(!ok) {volatile uint8_t *q=p;for(unsigned i=0;i<n;i++) q[i]=0;return -1;}
        previous=value;
        for(unsigned i=0;i<8 && at<n;i++,at++) p[at]=value>>(8*i);
    }
    return 0;
}
