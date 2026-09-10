#ifndef _ARM_CPU_FEATURES_H_
#define _ARM_CPU_FEATURES_H_

#include <stdint.h>
#include "include/types.h"

struct arm_cpu_features {
    bool     detected;
    bool     pan;          /* FEAT_PAN: EL1 cannot touch EL0-accessible memory */
    bool     el0_aarch32;
    bool     aes, sha256, crc32, atomics;
    uint64_t midr;
};

void                           arm_cpu_features_detect(void);
const struct arm_cpu_features *arm_cpu_features(void);

/* Per core: SCTLR_EL1.SPAN = 0 and PSTATE.PAN = 1. Same "CR4 is per core"
 * reasoning as x86 -- a core that skipped it is the one soft spot, and the
 * whole job of the feature is to be invisible while it works. */
void arm_protection_init_this_cpu(void);

uint64_t arm_read_sctlr(void);
uint64_t arm_read_pan(void);

/* SET PSTATE.PAN WITHOUT REQUIRING AN ARMv8.1 ASSEMBLER.
 *
 * `msr pan, #imm` is an ARMv8.1 mnemonic and this kernel builds with the
 * baseline -march, so the assembler would reject it -- and raising -march for
 * the whole kernel to get one instruction would let the compiler emit v8.1
 * instructions everywhere, on cores that may not have them. The encoding is
 * emitted directly instead:
 *
 *   MSR (immediate):  1101 0101 0000 0 op1 0100 CRm op2 11111
 *   PAN is op1=0, op2=4, and CRm carries the immediate.
 *   => 0xD500401F | (4 << 5) | (imm << 8)  =  0xD500409F | (imm << 8)
 *
 * A raw encoding is a thing to be suspicious of, so: PAN=1 is 0xD500419F and
 * PAN=0 is 0xD500409F, and `test hardening` reads PSTATE.PAN back afterwards
 * rather than trusting that these are right. */
#define ARM_SET_PAN(x) \
    __asm__ volatile(".inst %0" :: "i"(0xD500409Fu | (((x) & 1u) << 8)) : "memory")

#endif /* _ARM_CPU_FEATURES_H_ */
