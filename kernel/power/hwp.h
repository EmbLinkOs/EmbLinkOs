#ifndef EMBLINK_HWP_H
#define EMBLINK_HWP_H
#include <stdint.h>
/* Construct an autonomous request from the processor's own advertised bounds.
 * No clock-ratio guesses or fabricated frequencies. */
static inline int hwp_request(uint64_t capabilities,uint64_t previous,unsigned profile,int epp,uint64_t *out) {
    unsigned highest=capabilities&255,lowest=(capabilities>>24)&255;
    if(!lowest || !highest || lowest>highest || profile>2)return -1;
    uint64_t value=(previous&~(0xffffffffULL|(1ULL<<42)))|lowest|((uint64_t)highest<<8);
    if(epp)value|=(uint64_t)(profile==1?192:profile==2?0:128)<<24;
    else value|=previous&0xff000000;
    *out=value;return 0;
}
#endif
