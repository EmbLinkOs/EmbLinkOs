#ifndef _CPU_FEATURES_H_
#define _CPU_FEATURES_H_

#include <stdint.h>
#include "include/types.h"

/* ==========================================================================
 * WHAT THIS PROCESSOR CAN DO, asked once and remembered.
 *
 * CPUID was being issued ad hoc wherever someone needed an answer, which is
 * how a kernel ends up asking the same question four times and disagreeing
 * with itself once. This asks at boot, on the BSP, and every later question is
 * a field read.
 *
 * ONLY WHAT IS ACTED ON. A table of every CPUID bit is a table nobody
 * maintains; each field here exists because some code branches on it, and a
 * field that stops being read should be deleted rather than kept "for
 * completeness".
 * ========================================================================== */
struct cpu_features {
    bool detected;

    /* --- protection features, the reason this file exists ----------------- */
    bool smep;      /* CR4.SMEP: CPL 0 cannot EXECUTE a user page            */
    bool smap;      /* CR4.SMAP: CPL 0 cannot READ/WRITE one unless AC=1     */
    bool umip;      /* CR4.UMIP: SGDT/SIDT/SLDT/SMSW/STR fault in ring 3     */
    bool nx;        /* EFER.NXE: the no-execute bit in a PTE means it        */
    bool pge;       /* global pages -- kernel TLB entries survive a CR3 load */

    /* --- things other subsystems already ask about ------------------------ */
    bool invpcid;   /* selective TLB invalidation by ASID                    */
    bool pcid;      /* address-space IDs in CR3                              */
    bool tsc_deadline;
    bool rdrand;
    bool rdseed;
    bool fsgsbase;

    uint32_t max_basic_leaf;
    char     vendor[13];
    char     brand[49];
};

/* Ask the processor. Idempotent; called once from the BSP before anything
 * branches on the answers. */
void                       cpu_features_detect(void);
const struct cpu_features *cpu_features(void);

/* Turn on every protection this processor supports, on THIS core.
 *
 * PER-CORE, like EFER.NXE and the PAT: CR4 is not shared, and a core that
 * skipped this would be the one soft spot in an otherwise hardened machine --
 * with nothing to indicate it, because the protection's whole job is to be
 * invisible when it is working. Called from vmm_init() (BSP) and ap_main().
 *
 * Must run AFTER cpu_features_detect() and BEFORE any user process exists. */
void cpu_protection_init_this_cpu(void);

/* What is actually ON right now, read back from CR4 rather than from what we
 * believe we wrote. `test hardening` prints this, because "we set the bit" and
 * "the bit is set" are different claims and only the second one is evidence. */
uint64_t cpu_read_cr4(void);

#endif /* _CPU_FEATURES_H_ */
