#include "power/policy.h"
#include "power/hwp.h"
#include "include/percpu.h"
struct core_power {unsigned initialized,supported,epp,applied;uint64_t caps;};
static struct core_power cores[MAX_CPUS];
static unsigned selected;
#ifndef HWP_CPUID
static void cpuid(unsigned leaf,unsigned *a,unsigned *b,unsigned *c,unsigned *d) {
    __asm__ volatile("cpuid":"=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d):"a"(leaf),"c"(0));
}
#define HWP_CPUID cpuid
static uint64_t read_msr(unsigned reg) {
    uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(reg));return lo|((uint64_t)hi<<32);
}
static void write_msr(unsigned reg,uint64_t v) {
    __asm__ volatile("wrmsr"::"c"(reg),"a"((uint32_t)v),"d"((uint32_t)(v>>32)));
}
#define HWP_READ read_msr
#define HWP_WRITE write_msr
#endif
void cpu_power_poll(void) {
    unsigned index=this_cpu()->cpu_index;if(index>=MAX_CPUS)return;
    struct core_power *p=&cores[index];
    if(!p->initialized) {
        unsigned a,b,c,d;p->initialized=1;p->applied=UINT32_MAX;
        HWP_CPUID(0,&a,&b,&c,&d);
        if(a<6 || b!=0x756e6547 || d!=0x49656e69 || c!=0x6c65746e)return;
        HWP_CPUID(6,&a,&b,&c,&d);if(!(a&(1u<<7)))return;
        p->epp=(a&(1u<<10))!=0;p->caps=HWP_READ(0x771);
        uint64_t request;
        if(hwp_request(p->caps,HWP_READ(0x774),0,p->epp,&request))return;
        HWP_WRITE(0x770,HWP_READ(0x770)|1); /* advertised Intel HWP enable */
        __atomic_store_n(&p->supported,1,__ATOMIC_RELEASE);
    }
    unsigned profile=__atomic_load_n(&selected,__ATOMIC_ACQUIRE);
    if(!p->supported || p->applied==profile)return;
    uint64_t request;
    if(!hwp_request(p->caps,HWP_READ(0x774),profile,p->epp,&request)) {
        HWP_WRITE(0x774,request);p->applied=profile;
    }
}
unsigned cpu_power_count(void) {
    unsigned n=0;for(unsigned i=0;i<MAX_CPUS;i++)n+=(__atomic_load_n(&cores[i].supported,__ATOMIC_ACQUIRE)!=0 && cores[i].epp);return n;
}
void cpu_power_profile(unsigned profile) {__atomic_store_n(&selected,profile,__ATOMIC_RELEASE);}
