#include "mm/pmm.h"
#include "arch/aarch64/boot/fdt.h"
#include "boot/boot_protocol.h"
#include "include/kprintf.h"

/* The aarch64 half of pmm.h's arch hook. See the declaration there for why
 * this exists at all rather than pmm.c hardcoding an answer. */

/* Withhold a physical range from the allocator, rounded OUT to whole pages at
 * both ends. Rounding in would leave a partially-owned page allocatable, and
 * the corruption from that appears far from its cause. */
static void reserve_range(uint64_t base, uint64_t size, const char *why) {
    if (size == 0)
        return;

    uint64_t first = base & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last  = (base + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t p = first; p < last; p += PAGE_SIZE)
        pmm_reserve_page(p);

    kprintf("pmm: reserved %p..%p (%d pages) -- %s\n",
            (void *)(uintptr_t)first, (void *)(uintptr_t)last,
            (int)((last - first) / PAGE_SIZE), why);
}

void arch_pmm_reserve_fixed(void) {
    /* No trampoline. x86 has to reserve a fixed low page because INIT-SIPI-SIPI
     * jumps a secondary CPU to a physical address encoded in the SIPI vector,
     * so that address must be agreed in advance. PSCI CPU_ON takes the entry
     * point as a plain argument, so on this architecture there is nothing to
     * reserve for SMP. That is a fact about the architecture (docs/ARM64.md
     * §2.6), not a gap.
     *
     * There IS something to reserve, though, and it is easy to miss: the
     * DEVICE TREE. Firmware placed it inside the RAM that /memory reports as
     * usable, so pmm_init() marks those pages free, and unlike the kernel
     * image it does not sit below kernel_end where the blanket reservation
     * catches it. The first sizeable allocation would then overwrite the only
     * description of the machine -- and everything that reads it afterwards
     * (A3's GIC address, A7's PCIe window) would get plausible garbage.
     *
     * Plus whatever firmware itself asked us not to touch, from the blob's
     * memory reservation block. */

    if (!fdt_ok()) {
        kprintf("pmm: no device tree to protect -- nothing arch-reserved\n");
        return;
    }

    reserve_range(fdt_phys_base(), fdt_total_size(), "device tree");

    /* The blob's own memory reservation block: ranges firmware states must not
     * be touched, listed outside the tree entirely. */
    uint64_t ra = 0, rs = 0;
    for (uint32_t i = 0; fdt_mem_rsv(i, &ra, &rs); i++)
        reserve_range(ra, rs, "firmware reservation");

    /* And every RESERVED entry the device tree producer recorded -- today the
     * children of /reserved-memory. pmm_init() frees USABLE ranges and never
     * un-frees them, so a RESERVED range that overlaps a USABLE one (which is
     * exactly what /reserved-memory is) has to be taken back here. */
    uint32_t n = boot_mmap_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct boot_mmap_entry *e = boot_mmap_at(i);
        if (e && e->type != E820_USABLE)
            reserve_range(e->base, e->length, "device tree reserved range");
    }
}
