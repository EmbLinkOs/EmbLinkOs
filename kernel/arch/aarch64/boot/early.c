#include <stdint.h>
#include "arch/aarch64/drivers/pl011.h"

/* Early aarch64 bring-up entry -- docs/ARM64.md phase A0.
 *
 * This file is TEMPORARY BY DESIGN. It exists because kernel/main.c cannot run
 * yet: it wants a GDT, an IDT, a PIC and an APIC on its first page. As A1-A3
 * land exceptions, the MMU and the GIC, the work here migrates into the shared
 * kernel/main.c behind the arch_* seam and this file shrinks to nothing. Do not
 * grow it into a second kernel entry point.
 *
 * Called from boot.S with x0 = the device tree pointer QEMU planted. */

/* Flat device tree header, the only two fields A0 needs. Both are big-endian
 * on every platform -- that is the format, not the machine -- so they are
 * byte-swapped rather than cast. (docs/ARM64.md §5: little-endian only. This
 * is not a counter-example; DTB is a wire format.) */
#define FDT_MAGIC 0xd00dfeedu

static uint32_t be32(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

static uint64_t read_currentel(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return v >> 2;
}

static uint64_t read_midr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
    return v;
}

static uint64_t read_sctlr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

/* Counter frequency, in Hz, as firmware programmed it. A3 builds the tick on
 * this; printing it now is a free check that boot.S's CNTHCTL_EL2 handling
 * left EL1 able to read the timer registers at all. */
static uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

extern char kernel_end[];

void arch_early_main(uint64_t dtb);

void arch_early_main(uint64_t dtb) {
    pl011_init();

    pl011_puts("\n");
    pl011_puts("EmbLinkOS aarch64 -- phase A0 (skeleton + console)\n");
    pl011_puts("  see docs/ARM64.md\n\n");

    pl011_puts("  CurrentEL   : EL");
    pl011_putdec(read_currentel());
    pl011_puts("\n");

    pl011_puts("  MIDR_EL1    : ");
    pl011_puthex64(read_midr());
    pl011_puts("\n");

    pl011_puts("  SCTLR_EL1   : ");
    pl011_puthex64(read_sctlr());
    pl011_puts("   (M=0 => MMU off, as A0 intends)\n");

    pl011_puts("  CNTFRQ_EL0  : ");
    pl011_putdec(read_cntfrq());
    pl011_puts(" Hz\n");

    pl011_puts("  kernel_end  : ");
    pl011_puthex64((uint64_t)(uintptr_t)kernel_end);
    pl011_puts("\n");

    /* The DTB check is the real content of A0. A banner only proves the UART
     * works; this proves the FIRMWARE HANDOFF works -- that x0 survived the
     * EL2 drop, the bss zeroing and the stack switch. A2 reads the memory map
     * from this same pointer, and diagnosing a bad x0 there, after the MMU is
     * on, is enormously harder than diagnosing it here. */
    pl011_puts("  DTB (x0)    : ");
    pl011_puthex64(dtb);
    if (dtb && be32((const void *)(uintptr_t)dtb) == FDT_MAGIC) {
        pl011_puts("   magic ok, ");
        pl011_putdec(be32((const void *)(uintptr_t)(dtb + 4)));
        pl011_puts(" bytes\n");
    } else {
        pl011_puts("   NO FDT MAGIC -- firmware handoff is wrong\n");
    }

    pl011_puts("\nA0 reached. Parking (no exception vectors until A1).\n");

    for (;;)
        __asm__ volatile("wfi");
}
