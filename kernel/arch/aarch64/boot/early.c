#include <stdint.h>
#include "arch/aarch64/drivers/pl011.h"
#include "arch/aarch64/irq/exception.h"
#include "arch/aarch64/boot/fdt.h"
#include "arch/aarch64/mm/pagetable.h"
#include "boot/boot_protocol.h"
#include "mm/pmm.h"
#include "include/kprintf.h"

/* Early aarch64 bring-up entry -- docs/ARM64.md phases A0-A2.
 *
 * This file is TEMPORARY BY DESIGN. It exists because kernel/main.c cannot run
 * yet: it wants a GDT, an IDT, a PIC and an APIC on its first page. As the
 * remaining phases land the GIC, the timer and user mode, the work here
 * migrates into the shared kernel/main.c behind the arch_* seam and this file
 * shrinks to nothing. Do not grow it into a second kernel entry point.
 *
 * Called from boot.S AFTER the MMU is on and the kernel has jumped to the
 * higher half, with x0 = the PHYSICAL device tree address QEMU planted. */

#define PL011_PHYS 0x09000000UL

static uint64_t read_sysreg_currentel(void) {
    uint64_t v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return v >> 2;
}
static uint64_t read_sysreg_midr(void) {
    uint64_t v; __asm__ volatile("mrs %0, midr_el1" : "=r"(v)); return v;
}
static uint64_t read_sysreg_sctlr(void) {
    uint64_t v; __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v)); return v;
}
static uint64_t read_sysreg_cntfrq(void) {
    uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

extern char kernel_end[];

/* --- self-test -------------------------------------------------------------
 *
 * A fault handler that has never fired is a fault handler that does not work,
 * and "it booted" is no evidence about code that only runs when something goes
 * wrong. So the kernel deliberately breaks itself on the way up and recovers.
 *
 * exception_probe() makes synchronous faults recoverable for the duration of
 * one call (see exception.h). It was not built for this test -- it is the
 * shape a memory prober needs -- but the test is what proves it. */

static unsigned selftest_fails;

/* A software breakpoint. ELR points AT the brk, so the handler's +4 lands on
 * the next instruction. EC decodes as 0x3C. */
static void fault_brk(void) {
    __asm__ volatile("brk #0");
}

/* An unaligned 64-bit load from the UART's MMIO window.
 *
 * At A1 this test used ordinary RAM, because with the MMU off the architecture
 * treats every access as Device memory and unaligned Device accesses are
 * illegal. A2 turned the MMU on and mapped RAM as Normal, where unaligned is
 * perfectly legal -- so the old test stopped faulting, exactly as predicted.
 * Aiming it at a genuinely Device-mapped address restores the guarantee and,
 * better, now tests the thing that is actually true: the MMIO window really is
 * Device-nGnRnE and not accidentally Normal. */
static void fault_unaligned(void) {
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(MMIO_BASE + PL011_PHYS + 0x19);
    volatile uint64_t sink;
    sink = *p;
    (void)sink;
}

/* A read from a low address, after the identity map has been dropped. TTBR0 is
 * now completely empty, so this is a level-0 translation fault -- and it is
 * also what a null pointer dereference does from here on, which is the point:
 * before A2 dropped the identity map, *(int *)0x40200000 read real memory. */
static void fault_unmapped(void) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)0x40200000UL;
    volatile uint32_t sink;
    sink = *p;
    (void)sink;
}

/* A write to .rodata. This can only fault because vm_protect_kernel_sections()
 * ran: until then the whole kernel lived in one RWX 1 GiB block. It is the
 * single most direct evidence that the permissions are real. */
extern char __rodata_start[];
static void fault_rodata_write(void) {
    volatile char *p = (volatile char *)__rodata_start;
    *p = 0x55;
}

static void expect(const char *what, unsigned got, unsigned want) {
    if (got != want)
        selftest_fails++;
    kprintf("  [%s] %s  (%d caught, expected %d)\n",
            got == want ? " ok " : "FAIL", what, (int)got, (int)want);
}

static void selftest_faults(void) {
    kprintf("\n--- self-test: deliberately faulting ---\n");
    expect("brk #0 -> breakpoint, recovered",
           exception_probe(fault_brk, 1), 1);
    expect("unaligned load on Device memory -> alignment fault",
           exception_probe(fault_unaligned, 1), 1);
    expect("read of unmapped low VA -> translation fault",
           exception_probe(fault_unmapped, 1), 1);
    expect("write to .rodata -> permission fault",
           exception_probe(fault_rodata_write, 1), 1);
}

/* Does the allocator the device tree just fed actually work? Allocating,
 * writing through the direct map, reading back and freeing exercises the DTB
 * memory map, pmm's bitmap, and the direct-map mapping in one go -- any of
 * which being wrong would otherwise show up much later as corruption. */
static void selftest_pmm(void) {
    kprintf("\n--- self-test: physical allocator ---\n");

    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();

    if (!a || !b || a == b) {
        kprintf("  [FAIL] pmm_alloc_page returned %p / %p\n",
                (void *)(uintptr_t)a, (void *)(uintptr_t)b);
        selftest_fails++;
        return;
    }

    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)P2V(a);
    *p = 0xA2A2A2A2A2A2A2A2ULL;

    if (*p != 0xA2A2A2A2A2A2A2A2ULL) {
        kprintf("  [FAIL] direct-map readback of %p wrong\n", (void *)(uintptr_t)a);
        selftest_fails++;
    } else if (vm_translate(P2V(a)) != a) {
        kprintf("  [FAIL] direct map does not translate %p back to %p\n",
                (void *)(uintptr_t)P2V(a), (void *)(uintptr_t)a);
        selftest_fails++;
    } else {
        kprintf("  [ ok ] allocated %p and %p, wrote and read back via the direct map\n",
                (void *)(uintptr_t)a, (void *)(uintptr_t)b);
    }

    pmm_free_page(a);
    pmm_free_page(b);
}

void arch_early_main(uint64_t dtb_phys);

void arch_early_main(uint64_t dtb_phys) {
    /* The UART still answers at its physical address -- TTBR0's identity map is
     * alive -- but that map is about to go away, so move to the MMIO window
     * before printing anything we would miss. */
    pl011_init();
    pl011_use_mmio_window(MMIO_BASE + PL011_PHYS);

    kprintf("\nEmbLinkOS aarch64 -- phase A2 (MMU, higher half, device tree)\n");
    kprintf("  see docs/ARM64.md\n\n");

    kprintf("  CurrentEL   : EL%d\n",   (int)read_sysreg_currentel());
    kprintf("  MIDR_EL1    : %p\n",     (void *)(uintptr_t)read_sysreg_midr());
    kprintf("  SCTLR_EL1   : %p   (M=1 => MMU ON)\n",
            (void *)(uintptr_t)read_sysreg_sctlr());
    kprintf("  CNTFRQ_EL0  : %d Hz\n",  (int)read_sysreg_cntfrq());

    /* The A2 claim, stated as an address rather than an assertion: this code's
     * own location. Anything below 0xFFFF... means the jump to the higher half
     * did not happen and everything after it is a coincidence. */
    kprintf("  running at  : %p  (higher half: %s)\n",
            (void *)(uintptr_t)&arch_early_main,
            ((uint64_t)(uintptr_t)&arch_early_main >= KERNEL_VIRTUAL_BASE)
                ? "YES" : "NO -- A2 FAILED");
    kprintf("  kernel_end  : %p -> phys %p\n",
            (void *)kernel_end, (void *)(uintptr_t)KV2P((uint64_t)(uintptr_t)kernel_end));
    kprintf("  DTB (x0)    : %p\n", (void *)(uintptr_t)dtb_phys);

    exception_init();
    kprintf("  VBAR_EL1 installed -- faults are decoded, not fatal silence.\n\n");

    /* Firmware handoff -> memory map -> allocator. Each step is useless
     * without the one before it, and each is fatal on its own if it fails,
     * which is why none of them has a fallback path. */
    boot_protocol_capture(dtb_phys);
    boot_protocol_dump();
    pmm_init();

    /* Take the boot tables over and make them describe reality: per-section
     * permissions instead of one RWX gigabyte, and no identity map. */
    kprintf("\n");
    vm_init();
    if (vm_protect_kernel_sections() != PT_OK) {
        kprintf("vm: FATAL could not apply kernel section permissions\n");
        for (;;) __asm__ volatile("wfi");
    }
    vm_drop_identity_map();
    kprintf("vm: identity map dropped -- low addresses now fault\n");
    vm_dump_kernel_mapping();

    selftest_pmm();
    selftest_faults();

    kprintf("\n--- self-test done: %d failure(s) ---\n", (int)selftest_fails);
    pmm_print_stats();

    kprintf("\nA2 reached. Parking (no interrupt controller until A3).\n");

    for (;;)
        __asm__ volatile("wfi");
}
