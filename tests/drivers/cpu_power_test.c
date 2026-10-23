#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#define __PERCPU_H__
#define MAX_CPUS 3
struct cpu_data {unsigned cpu_index;};
static struct cpu_data current_cpu;
static struct cpu_data *this_cpu(void){return &current_cpu;}
static uint64_t caps=(20ULL<<24)|(120ULL<<8)|160,request[3],enabled[3];
static unsigned reads,writes,vendor=1,features=(1u<<7)|(1u<<10);
static void cpuid_model(unsigned leaf,unsigned *a,unsigned *b,unsigned *c,unsigned *d){
    *a=*b=*c=*d=0;
    if(!leaf){*a=6;if(vendor){*b=0x756e6547;*d=0x49656e69;*c=0x6c65746e;}}
    else if(leaf==6)*a=features;
}
static uint64_t read_model(unsigned r){reads++;if(r==0x771)return caps;if(r==0x774)return request[current_cpu.cpu_index];assert(r==0x770);return enabled[current_cpu.cpu_index];}
static void write_model(unsigned r,uint64_t v){writes++;if(r==0x770)enabled[current_cpu.cpu_index]=v;else{assert(r==0x774);request[current_cpu.cpu_index]=v;}}
#define HWP_CPUID cpuid_model
#define HWP_READ read_model
#define HWP_WRITE write_model
#include "power/cpu.c"
int main(void){
    request[0]=(1ULL<<42)|(7ULL<<32);cpu_power_poll();assert(enabled[0]&1);
    assert((request[0]&0xffffffffu)==((128u<<24)|(160u<<8)|20u) && !(request[0]&(1ULL<<42)));
    assert((request[0]&(0x3ffULL<<32))==(7ULL<<32));unsigned old=writes;cpu_power_poll();assert(writes==old);
    cpu_power_profile(1);cpu_power_poll();assert(((request[0]>>24)&255)==192);
    current_cpu.cpu_index=1;cpu_power_poll();assert(cpu_power_count()==2 && ((request[1]>>24)&255)==192);
    current_cpu.cpu_index=2;vendor=0;unsigned old_reads=reads;cpu_power_poll();assert(reads==old_reads);
    uint64_t out;assert(hwp_request(1ULL<<24,0,0,1,&out)<0);
    assert(hwp_request((200ULL<<24)|100,0,0,1,&out)<0);
    assert(hwp_request(caps,0,3,1,&out)<0);
    puts("CPU power: Intel CPUID gate, per-core autonomous HWP/EPP, bounds, preserved window and non-Intel refusal OK");
}
