#include "arch/aarch64/cpu/cpu_features.h"
#include "include/kprintf.h"

/* ==========================================================================
 * WHAT THIS PROCESSOR CAN ENFORCE -- the aarch64 half.
 *
 * Same purpose as the x86 file of this name, different questions. There is no
 * CPUID here: an AArch64 core describes itself through the ID_AA64* registers,
 * which are readable at EL1 and are architecturally required to exist. Each
 * field is a small integer where 0 means "not implemented" and larger numbers
 * mean successive revisions -- so every check below is "!= 0", never "== 1",
 * because a core with a newer version of a feature still has the feature.
 * ========================================================================== */

static struct arm_cpu_features g_cf;

#define SCTLR_SPAN (1ULL << 23)   /* 0 = set PSTATE.PAN on exception entry */

static inline uint64_t read_sctlr(void) {
    uint64_t v; __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v)); return v;
}
static inline void write_sctlr(uint64_t v) {
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(v));
    __asm__ volatile("isb" ::: "memory");
}

uint64_t arm_read_sctlr(void) { return read_sctlr(); }

uint64_t arm_read_pan(void) {
    /* PSTATE is not a register you can read whole; PAN comes back through
     * the "current PSTATE" accessor, where it sits at bit 22. */
    uint64_t v;
    __asm__ volatile("mrs %0, pan" : "=r"(v));
    return (v >> 22) & 1;
}

void arm_cpu_features_detect(void) {
    if (g_cf.detected) return;

    uint64_t mmfr1;
    __asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    g_cf.pan = ((mmfr1 >> 20) & 0xF) != 0;      /* FEAT_PAN, ARMv8.1 */

    uint64_t pfr0;
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
    g_cf.el0_aarch32 = ((pfr0 >> 0) & 0xF) == 2;

    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    g_cf.aes    = ((isar0 >> 4)  & 0xF) != 0;
    g_cf.sha256 = ((isar0 >> 12) & 0xF) != 0;
    g_cf.crc32  = ((isar0 >> 16) & 0xF) != 0;
    g_cf.atomics= ((isar0 >> 20) & 0xF) != 0;
    g_cf.rndr   = ((isar0 >> 60) & 0xF) != 0;     /* FEAT_RNG, ARMv8.5 */

    __asm__ volatile("mrs %0, midr_el1" : "=r"(g_cf.midr));

    g_cf.detected = true;

    kprintf("cpu: MIDR 0x%llx  protection available:%s  crypto:%s%s%s\n",
            (unsigned long long)g_cf.midr,
            g_cf.pan ? " PAN" : " (no PAN)",
            g_cf.aes ? " AES" : "", g_cf.sha256 ? " SHA256" : "",
            g_cf.crc32 ? " CRC32" : "");
    kprintf("cpu: hardware RNG (RNDR): %s\n", g_cf.rndr ? "yes" : "no");
    if (!g_cf.pan)
        kprintf("cpu: NOTE -- no FEAT_PAN; the kernel runs without it rather "
                "than refusing to boot. PXN still holds (EL1 cannot EXECUTE "
                "user memory); what is missing is the READ/WRITE half.\n");
}

const struct arm_cpu_features *arm_cpu_features(void) { return &g_cf; }

void arm_protection_init_this_cpu(void) {
    if (!g_cf.detected || !g_cf.pan) return;

    /* SPAN = 0: the processor SETS PSTATE.PAN on every exception entry to EL1.
     *
     * This is the bit that makes PAN a default rather than a discipline.
     * Without it, a syscall would inherit whatever PAN state happened to be
     * live -- including PAN=0 left behind by an earlier deliberate access --
     * and the protection would hold only as long as nobody made a mistake,
     * which is the situation it exists to replace. With it, every entry to the
     * kernel starts forbidden, and permission is something the kernel has to
     * ask for, once, around the access that needs it. */
    uint64_t sctlr = read_sctlr();
    sctlr &= ~SCTLR_SPAN;
    write_sctlr(sctlr);

    /* And forbid it RIGHT NOW, for the boot path, which is already at EL1 and
     * did not arrive by an exception. */
    ARM_SET_PAN(1);
}

/* ==========================================================================
 * Hardware entropy for lib/random.c.
 * ========================================================================== */
#include "lib/random.h"
#include "drivers/timer/timer.h"

/* RNDR (FEAT_RNG, ARMv8.5). Read through its system-register encoding,
 * S3_3_C2_C4_0, rather than the `rndr` mnemonic, for the same reason PAN is
 * set by encoding: the mnemonic needs an -march the rest of the kernel does not
 * want. Executing it on a core without the feature is UNDEFINED, so the
 * ID_AA64ISAR0_EL1.RNDR field is checked first, and that check is the only
 * thing that makes this safe to call at all.
 *
 * RNDR reports through the flags, not through a return value: on success it
 * writes the value and sets NZCV to 0b0000; when it cannot produce one it sets
 * NZCV to 0b0100 -- Z set. So "Z clear" is the success test. */
bool arch_hw_random_u64(uint64_t *out) {
    if (!g_cf.rndr) return false;
    for (int i = 0; i < 16; i++) {
        uint64_t v, nzcv;
        __asm__ volatile("mrs %0, S3_3_C2_C4_0\n\tmrs %1, nzcv"
                         : "=r"(v), "=r"(nzcv) :: "cc");
        if (!(nzcv & (1ull << 30))) {   /* Z clear: a value was produced */
            *out = v;
            return true;
        }
    }
    return false;
}

void arch_random_seed_extra(void (*sink)(const void *, size_t)) {
    uint64_t v;
    v = time_get_ns();                                     sink(&v, sizeof v);
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));      sink(&v, sizeof v);
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));      sink(&v, sizeof v);
}
