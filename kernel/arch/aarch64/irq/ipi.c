#include "include/arch_ipi.h"
#include "arch/aarch64/smp/smp.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/cpu/percpu.h"
#include "include/kprintf.h"
#include "mm/vmm.h"

/* The aarch64 half of include/arch_ipi.h.
 *
 * An SGI ("software generated interrupt") is INTID 0-15, and unlike an x86 IPI
 * it is not a vector in a shared table: SGIs are per-core in the GIC exactly
 * as PPIs are, so each core enables its own. Sending one is a system-register
 * write -- ICC_SGI1R_EL1 -- with no memory-mapped controller involved at all,
 * which is the same trade the rest of GICv3 makes.
 *
 * ONE THING THIS DOES NOT HAVE TO CARRY, and it is the interesting one:
 * IPI_TLB_SHOOTDOWN is never sent here. `tlbi ... is` broadcasts to the whole
 * inner-shareable domain in HARDWARE, so a page unmapped on one core is
 * already invalid on all of them by the time the `dsb ish` retires. x86 has to
 * send an interrupt and hope; this architecture simply does not have the
 * problem. The reason still exists in the enum because the SHARED code is
 * written against one set of meanings -- and because a machine with cores in
 * different shareability domains would need it.
 */

/* IRM = 1 in ICC_SGI1R_EL1 means "every core except this one", which is the
 * only routing mode used here -- the same shorthand x86's ICR has, and for the
 * same reason: it cannot miss a core that came up after a list was built. */
#define SGI_IRM_ALL_BUT_SELF (1ULL << 40)

#define SGI_INTID(reason) ((uint32_t)(reason))   /* SGI 0, 1, 2 */

static void sgi_handler(uint32_t intid) {
    /* INTID IS the reason: SGIs 0..2 map one-to-one onto enum ipi_reason.
     * gic_dispatch() has already claimed the interrupt and will EOI it after
     * this returns -- except for IPI_HALT, which never returns, and that is
     * correct: a core parking on a panic has no use for a tidy EOI. */
    ipi_dispatch((enum ipi_reason)intid);
}

void arch_ipi_init_this_cpu(void) {
    for (uint32_t r = 0; r < IPI_REASON_COUNT; r++) {
        gic_register(SGI_INTID(r), sgi_handler, "IPI");
        gic_enable(SGI_INTID(r));
    }
}

bool arch_ipi_broadcast(enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT || cpu_count <= 1)
        return false;

    /* Bits 27:24 are the INTID; IRM selects "all but self". Affinity fields
     * are ignored with IRM set, which is why none are filled in. */
    uint64_t val = SGI_IRM_ALL_BUT_SELF |
                   ((uint64_t)SGI_INTID(reason) << 24);

    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("msr S3_0_C12_C11_5, %0" :: "r"(val));   /* ICC_SGI1R_EL1 */
    __asm__ volatile("isb" ::: "memory");
    return true;
}

/* ONE core, by dense cpu index. IRM=0 means "use the affinity fields", and the
 * target is a 16-bit MASK of cores within one Aff1 cluster -- so this builds
 * the mask from that core's own MPIDR rather than from its index.
 *
 * Refusing a target outside this cluster rather than silently sending to the
 * wrong core: on `virt` every core shares Aff1..Aff3, so the case does not
 * arise here, and a machine where it does would otherwise wake a stranger. */
bool arch_ipi_send(uint32_t cpu, enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT || cpu >= cpu_count)
        return false;
    if (cpu == this_cpu()->cpu_index)
        return false;

    uint64_t self = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
    uint64_t tgt = smp_cpu_mpidr(cpu);

    uint64_t aff1 = (tgt >> 8)  & 0xFF;
    uint64_t aff2 = (tgt >> 16) & 0xFF;
    uint64_t aff3 = (tgt >> 32) & 0xFF;
    if (aff1 != ((self >> 8) & 0xFF) || aff2 != ((self >> 16) & 0xFF) ||
        aff3 != ((self >> 32) & 0xFF))
        return false;                     /* another cluster: not ours to target */

    uint64_t aff0 = tgt & 0xFF;
    if (aff0 > 15)
        return false;                     /* outside this target list's 16 bits */

    uint64_t val = ((uint64_t)SGI_INTID(reason) << 24) |
                   (aff3 << 48) | (aff2 << 32) | (aff1 << 16) |
                   (1ULL << aff0);        /* IRM = 0: use the affinity fields */

    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("msr S3_0_C12_C11_5, %0" :: "r"(val));   /* ICC_SGI1R_EL1 */
    __asm__ volatile("isb" ::: "memory");
    return true;
}

/* This core's TLB only. Present for the shared IPI handler's sake; nothing on
 * this architecture actually needs it, because the broadcast happens in
 * hardware -- see the note at the top. */
void arch_tlb_flush_all_local(void) {
    __asm__ volatile("dsb ishst" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
