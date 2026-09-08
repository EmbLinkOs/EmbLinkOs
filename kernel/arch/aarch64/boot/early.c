#include <stdint.h>
#include "arch/aarch64/drivers/pl011.h"
#include "arch/aarch64/irq/exception.h"

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

/* --- A1 self-test ----------------------------------------------------------
 *
 * A fault handler that has never fired is a fault handler that does not work,
 * and "it booted" is not evidence about code that only runs when something
 * goes wrong. So A1 deliberately breaks things, three ways, and recovers.
 *
 * exception_probe() makes synchronous faults recoverable for the duration of
 * one function (see exception.h). That mechanism is not built for this test --
 * A2 needs exactly it to ask "is there memory at this address?" while walking
 * the device tree -- but the test is what proves it before A2 depends on it. */

/* A software breakpoint. ELR points AT the brk, so the handler's +4 lands on
 * the next instruction. EC should decode as 0x3C. */
static void fault_brk(void) {
    __asm__ volatile("brk #0");
}

/* An unaligned 64-bit load. This faults for a reason specific to where we are
 * in the campaign: with the MMU OFF, the architecture treats every access as
 * Device-nGnRnE memory, and unaligned Device accesses are not permitted. So
 * this is a guaranteed data abort with a FAR, at A1, without a single page
 * table existing yet -- which is exactly what is needed to prove FAR/FSC
 * decoding before A2 starts producing translation faults for real.
 *
 * (It will STOP faulting at A2, once this range is mapped Normal memory. That
 * is not a regression; it is the test noticing that the MMU turned on.) */
static void fault_unaligned(void) {
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)0x40200003UL;
    volatile uint64_t sink;
    sink = *p;
    (void)sink;
}

/* A read from a physical address `virt` has nothing behind it. Informational
 * only: QEMU may either raise an external abort or quietly return zero, and
 * which one it does is worth KNOWING before A2 writes a memory prober that
 * assumes an answer. */
static void fault_unassigned(void) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)0x0E000000UL;
    volatile uint32_t sink;
    sink = *p;
    (void)sink;
}

static void report(const char *what, unsigned n, unsigned expect) {
    pl011_puts(n == expect ? "  [ ok ] " : "  [FAIL] ");
    pl011_puts(what);
    pl011_puts("  (");
    pl011_putdec(n);
    pl011_puts(" caught, expected ");
    pl011_putdec(expect);
    pl011_puts(")\n");
}

static void a1_selftest(void) {
    unsigned n;

    pl011_puts("\n--- A1 self-test: deliberately faulting ---\n");

    n = exception_probe(fault_brk, 1);
    report("brk #0 -> breakpoint, recovered", n, 1);

    n = exception_probe(fault_unaligned, 1);
    report("unaligned load -> data abort with FAR, recovered", n, 1);

    /* Quiet, and not asserted: this one is a question, not a claim. */
    n = exception_probe(fault_unassigned, 0);
    pl011_puts("  [info] read of unassigned physical 0x0E000000: ");
    if (n) {
        pl011_puts("aborted (QEMU raises an external abort)\n");
    } else {
        pl011_puts("NO fault -- QEMU returns 0 for unassigned reads.\n");
        pl011_puts("         A2 cannot probe for RAM by reading; use the DTB memory node.\n");
    }

    pl011_puts("--- self-test done, kernel still running ---\n");
}

void arch_early_main(uint64_t dtb);

void arch_early_main(uint64_t dtb) {
    pl011_init();

    pl011_puts("\n");
    pl011_puts("EmbLinkOS aarch64 -- phase A1 (console + exception vectors)\n");
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

    /* --- A1: exception vectors ---------------------------------------------
     *
     * Installed here, as early as there is a console to report through,
     * because until VBAR_EL1 is set ANY fault jumps to whatever it happens to
     * hold -- zero on a cold `virt` -- and the symptom is an unexplained hang
     * with no output. Everything after this point fails LOUDLY. */
    exception_init();
    pl011_puts("\n  VBAR_EL1 installed -- faults are now decoded, not fatal silence.\n");

    a1_selftest();

    pl011_puts("\nA1 reached. Parking (no scheduler until A3).\n");

    for (;;)
        __asm__ volatile("wfi");
}
