#include "arch/x86_64/cpu/cpu_features.h"
#include "include/kprintf.h"
#include "include/kstring.h"

/* ==========================================================================
 * CPUID, once.
 * ========================================================================== */

#define CR4_PGE     (1ULL << 7)
#define CR4_PCIDE   (1ULL << 17)
#define CR4_SMEP    (1ULL << 20)
#define CR4_SMAP    (1ULL << 21)
#define CR4_UMIP    (1ULL << 11)

static struct cpu_features g_cf;

static inline void cpuid_count(uint32_t leaf, uint32_t sub,
                               uint32_t *a, uint32_t *b,
                               uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

uint64_t cpu_read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

void cpu_features_detect(void) {
    if (g_cf.detected) return;

    uint32_t a, b, c, d;

    cpuid_count(0, 0, &a, &b, &c, &d);
    g_cf.max_basic_leaf = a;
    /* The vendor string arrives as EBX, EDX, ECX -- in that order, which is
     * not the register order and is the classic way to print "GenuntelineI". */
    *(uint32_t *)&g_cf.vendor[0] = b;
    *(uint32_t *)&g_cf.vendor[4] = d;
    *(uint32_t *)&g_cf.vendor[8] = c;
    g_cf.vendor[12] = '\0';

    if (g_cf.max_basic_leaf >= 1) {
        cpuid_count(1, 0, &a, &b, &c, &d);
        g_cf.pge          = (d >> 13) & 1;
        g_cf.pcid         = (c >> 17) & 1;
        g_cf.tsc_deadline = (c >> 24) & 1;
        g_cf.rdrand       = (c >> 30) & 1;
    }

    /* Leaf 7 is where the protection bits live, and it does not exist on an
     * old enough processor -- hence the guard rather than a bare CPUID. */
    if (g_cf.max_basic_leaf >= 7) {
        cpuid_count(7, 0, &a, &b, &c, &d);
        g_cf.fsgsbase = (b >>  0) & 1;
        g_cf.smep     = (b >>  7) & 1;
        g_cf.invpcid  = (b >> 10) & 1;
        g_cf.rdseed   = (b >> 18) & 1;
        g_cf.smap     = (b >> 20) & 1;
        g_cf.umip     = (c >>  2) & 1;
    }

    /* NX lives in the extended leaves. */
    cpuid_count(0x80000000u, 0, &a, &b, &c, &d);
    uint32_t max_ext = a;
    if (max_ext >= 0x80000001u) {
        cpuid_count(0x80000001u, 0, &a, &b, &c, &d);
        g_cf.nx = (d >> 20) & 1;
    }
    if (max_ext >= 0x80000004u) {
        uint32_t *w = (uint32_t *)g_cf.brand;
        for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid_count(leaf, 0, &a, &b, &c, &d);
            *w++ = a; *w++ = b; *w++ = c; *w++ = d;
        }
        g_cf.brand[48] = '\0';
    }

    g_cf.detected = true;

    kprintf("cpu: %s%s%s\n", g_cf.vendor,
            g_cf.brand[0] ? " -- " : "", g_cf.brand);
    kprintf("cpu: protection available:%s%s%s%s\n",
            g_cf.smep ? " SMEP" : "", g_cf.smap ? " SMAP" : "",
            g_cf.umip ? " UMIP" : "", g_cf.nx   ? " NX"   : "");
    if (!g_cf.smep || !g_cf.smap)
        kprintf("cpu: NOTE -- this processor lacks%s%s; the kernel runs "
                "without that protection rather than refusing to boot\n",
                g_cf.smep ? "" : " SMEP", g_cf.smap ? "" : " SMAP");
}

const struct cpu_features *cpu_features(void) { return &g_cf; }

void cpu_protection_init_this_cpu(void) {
    if (!g_cf.detected) return;

    uint64_t cr4 = cpu_read_cr4();

    /* SMEP -- THE KERNEL MAY NOT EXECUTE USER PAGES.
     *
     * The classic escalation is to get the kernel to jump to an address the
     * attacker controls, and the easiest such address is one they wrote
     * themselves in their own process. SMEP makes that a fault instead of a
     * privilege escalation. It costs nothing and needs no code changes,
     * because a correct kernel never does this on purpose. */
    if (g_cf.smep) cr4 |= CR4_SMEP;

    /* SMAP -- THE KERNEL MAY NOT READ OR WRITE USER PAGES, unless it says so.
     *
     * This one DOES need code changes, and they are already in place: every
     * legitimate touch goes through copy_from_user/copy_to_user, which bracket
     * the access with stac/clac (uaccess_guard.h). Anything that reaches user
     * memory without going through them was a latent bug -- a kernel
     * dereferencing a pointer a user program chose -- and now faults loudly at
     * the moment it does it, rather than silently working until someone
     * arranges for the pointer to be interesting.
     *
     * That is the actual value: not that SMAP stops an exploit, but that it
     * turns "we believe all user access is centralised" into something the
     * hardware checks on every instruction. */
    if (g_cf.smap) cr4 |= CR4_SMAP;

    /* UMIP -- ring 3 may not ask where the descriptor tables are.
     *
     * SGDT/SIDT/SLDT/SMSW/STR are unprivileged by historical accident and
     * leak kernel addresses, which is the first thing an exploit wants. No
     * program on this OS has any use for them. */
    if (g_cf.umip) cr4 |= CR4_UMIP;

    /* Global pages: kernel mappings survive the CR3 reload on every address
     * space switch. A performance bit, not a protection one, and it is here
     * because it belongs to the same register and the same "once per core". */
    if (g_cf.pge) cr4 |= CR4_PGE;

    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}
