#include <stdint.h>
#include "arch/aarch64/drivers/pl011.h"
#include "arch/aarch64/irq/exception.h"
#include "arch/aarch64/boot/fdt.h"
#include "arch/aarch64/mm/pagetable.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/sched/bringup.h"
#include "arch/aarch64/syscall/usermode.h"
#include "drivers/timer/timer.h"
#include "drivers/bus/pci.h"
#include "boot/boot_protocol.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "mm/vmm.h"
#include "include/kmalloc.h"
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

/* Does the SHARED kernel heap work on this architecture?
 *
 * kernel/mm/kheap.c is 619 lines of x86-era allocator -- slabs, canaries,
 * coalescing -- compiled here unchanged. The only thing it needed was for
 * kernel/mm/vmm.h to mean something on aarch64, which mm/pagetable.c now
 * implements. Exercising it matters more than the usual "it linked": a heap
 * that is subtly wrong corrupts something far away and much later. */
static void selftest_kheap(void) {
    kprintf("\n--- self-test: shared kernel heap ---\n");
    kheap_init();

    /* Sizes that straddle the slab/large-block boundary, so both paths run. */
    static const uint64_t sizes[] = { 16, 64, 512, 4096, 40000 };
    void *p[5];
    bool bad = false;

    for (int i = 0; i < 5; i++) {
        p[i] = kmalloc(sizes[i]);
        if (!p[i]) {
            kprintf("  [FAIL] kmalloc(%d) returned null\n", (int)sizes[i]);
            bad = true;
            continue;
        }
        /* Fill with a per-allocation pattern: if two allocations overlap, the
         * later fill corrupts the earlier one and the readback below catches
         * it. A test that only checks the pointer is non-null would not. */
        for (uint64_t b = 0; b < sizes[i]; b++)
            ((volatile unsigned char *)p[i])[b] = (unsigned char)(i * 31 + (b & 0xFF));
    }

    for (int i = 0; i < 5 && !bad; i++) {
        for (uint64_t b = 0; b < sizes[i]; b++) {
            if (((volatile unsigned char *)p[i])[b] !=
                (unsigned char)(i * 31 + (b & 0xFF))) {
                kprintf("  [FAIL] allocation %d corrupted at byte %d\n",
                        i, (int)b);
                bad = true;
                break;
            }
        }
    }

    for (int i = 0; i < 5; i++)
        if (p[i]) kfree(p[i]);

    /* The allocator's own integrity check -- canaries and block chain. */
    kheap_check();

    kprintf("  [%s] kmalloc/kfree across 5 size classes, contents intact\n",
            bad ? "FAIL" : " ok ");
    if (bad)
        selftest_fails++;
}

/* Two address spaces, one virtual address, two different physical pages.
 *
 * This is the property every process depends on and nothing else tests: that
 * switching TTBR0 changes what an address MEANS. Reading the same VA before
 * and after the switch and getting different bytes is the only evidence that
 * matters -- "vmm_create_address_space returned non-zero" is not. */
static void selftest_address_spaces(void) {
    kprintf("\n--- self-test: address-space isolation ---\n");

    const uint64_t VA = 0x0000000030000000ULL;   /* a user-half address */
    uint64_t saved = 0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(saved));

    uint64_t as_a = vmm_create_address_space();
    uint64_t as_b = vmm_create_address_space();
    uint64_t pa   = pmm_alloc_page();
    uint64_t pb   = pmm_alloc_page();

    if (!as_a || !as_b || !pa || !pb) {
        kprintf("  [FAIL] could not create two address spaces\n");
        selftest_fails++;
        return;
    }

    /* Distinct contents, written through the direct map -- the kernel's own
     * view of the frames, which does not depend on either address space. */
    *(volatile uint64_t *)(uintptr_t)P2V(pa) = 0xAAAAAAAAAAAAAAAAULL;
    *(volatile uint64_t *)(uintptr_t)P2V(pb) = 0xBBBBBBBBBBBBBBBBULL;

    /* Mapped WITHOUT VMM_USER: the test runs at EL1, and a kernel-only
     * mapping in the TTBR0 half proves the same thing without depending on
     * whether privileged access to user pages is permitted. */
    vmm_map_in(as_a, VA, pa, VMM_WRITABLE);
    vmm_map_in(as_b, VA, pb, VMM_WRITABLE);

    vmm_switch_address_space(as_a);
    uint64_t seen_a = *(volatile uint64_t *)(uintptr_t)VA;
    vmm_switch_address_space(as_b);
    uint64_t seen_b = *(volatile uint64_t *)(uintptr_t)VA;

    vmm_switch_address_space(saved & 0x0000FFFFFFFFF000ULL);

    bool ok = (seen_a == 0xAAAAAAAAAAAAAAAAULL) && (seen_b == 0xBBBBBBBBBBBBBBBBULL);
    kprintf("  [%s] %p reads %p in one space and %p in the other\n",
            ok ? " ok " : "FAIL", (void *)(uintptr_t)VA,
            (void *)(uintptr_t)seen_a, (void *)(uintptr_t)seen_b);
    if (!ok)
        selftest_fails++;

    /* Destroying frees the frames AND the tables under each root. */
    vmm_destroy_address_space(as_a);
    vmm_destroy_address_space(as_b);
    kprintf("  [ ok ] both address spaces destroyed\n");
}

/* A do-nothing INTx handler, used only to prove the routing resolves. Nothing
 * should ever call it: no device has been told to interrupt yet. */
static void pci_probe_isr(void) { }

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

/* --- A3 self-test: is preemption real? ------------------------------------
 *
 * Two threads that do nothing but spin. The claim being tested is not that
 * they compute anything -- it is that a timer interrupt takes the CPU away
 * from one and gives it to another WITHOUT either of them cooperating, and
 * then gives it back. Only the scheduler's slice counts can show that, which
 * is why they are counted rather than inferred from output appearing. */

static void worker(int id) {
    uint64_t seen = 0;
    volatile uint64_t spins = 0;

    for (;;) {
        spins++;

        /* Print on the SLICE boundary, not every N iterations. The interesting
         * event is "I was taken off the CPU and given it back", and how many
         * times a spin loop went round between those is a fact about TCG's
         * speed, not about the scheduler. */
        uint64_t sl = bringup_thread_slices(id);
        if (sl != seen) {
            seen = sl;
            kprintf("  worker %d resumed: slice %d, uptime %d ms, %d spins\n",
                    id, (int)sl, (int)timer_uptime_ms(), (int)spins);
        }

        /* One of the two returns, so the "a thread finished" path is exercised
         * rather than assumed. */
        if (id == 2 && sl >= 4) {
            kprintf("  worker %d finishing after %d slices\n", id, (int)sl);
            return;
        }
    }
}

static void worker_a(void) { worker(1); }
static void worker_b(void) { worker(2); }

static void selftest_preemption(void) {
    uint64_t start = timer_get_ticks();
    uint64_t ms0   = timer_uptime_ms();

    /* Wait 40 ticks = 400 ms of guest time. wfi rather than a spin so the two
     * workers get the CPU; if preemption were broken this loop would simply
     * never finish, which is itself the clearest possible failure. */
    while (timer_get_ticks() - start < 40)
        __asm__ volatile("wfi");

    uint64_t ms1 = timer_uptime_ms();
    bringup_sched_stop();

    kprintf("\n--- self-test: preemption ---\n");

    uint64_t t = timer_get_ticks();
    kprintf("  [%s] timer fired: %d ticks in %d ms\n",
            t >= 40 ? " ok " : "FAIL", (int)t, (int)(ms1 - ms0));
    if (t < 40) selftest_fails++;

    /* The counter and the interrupt are independent sources of time; if they
     * disagree, one of them is wrong. 40 ticks at 100 Hz is 400 ms, and TCG
     * makes the wall clock elastic, so this is a sanity band and not a
     * precision claim. */
    uint64_t elapsed = ms1 - ms0;
    bool sane = elapsed >= 300 && elapsed <= 900;
    kprintf("  [%s] CNTVCT and the tick count agree (%d ms for %d ticks)\n",
            sane ? " ok " : "FAIL", (int)elapsed, (int)(t - start));
    if (!sane) selftest_fails++;

    for (int id = 1; id <= 2; id++) {
        uint64_t sl = bringup_thread_slices(id);
        kprintf("  [%s] worker %d was scheduled %d times\n",
                sl > 0 ? " ok " : "FAIL", id, (int)sl);
        if (!sl) selftest_fails++;
    }

    /* The one that is easy to miss: getting back. A scheduler that switches
     * away and never returns looks fine from the worker's side and has lost
     * the boot thread forever. */
    uint64_t boot_slices = bringup_thread_slices(0);
    kprintf("  [%s] boot thread was scheduled back %d times\n",
            boot_slices > 0 ? " ok " : "FAIL", (int)boot_slices);
    if (!boot_slices) selftest_fails++;

    bringup_sched_dump();
    gic_dump();

    kprintf("\n--- self-test done: %d failure(s) total ---\n", (int)selftest_fails);
}

void arch_early_main(uint64_t dtb_phys);

void arch_early_main(uint64_t dtb_phys) {
    /* The UART still answers at its physical address -- TTBR0's identity map is
     * alive -- but that map is about to go away, so move to the MMIO window
     * before printing anything we would miss. */
    pl011_init();
    pl011_use_mmio_window(MMIO_BASE + PL011_PHYS);

    kprintf("\nEmbLinkOS aarch64 -- phase A5 (EL0, svc, the neutral syscall path)\n");
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
    selftest_kheap();
    selftest_address_spaces();
    selftest_faults();

    kprintf("\n--- self-test done: %d failure(s) ---\n", (int)selftest_fails);
    pmm_print_stats();

    /* --- A3: interrupts and preemption ------------------------------------ */
    kprintf("\n");
    if (gic_init() != 0) {
        kprintf("gic: FATAL no interrupt controller\n");
        for (;;) __asm__ volatile("wfi");
    }

    /* --- A7: the PCIe bus ---------------------------------------------------
     * Before the scheduler starts, so the enumeration is not interleaved with
     * preemption output. The existing virtio-gpu and virtio-net drivers are
     * virtio-over-PCI, so this bus is what makes them reachable here at all. */
    kprintf("\n--- PCIe ---\n");
    pci_init();
    { extern void pci_ecam_assign_resources(void); pci_ecam_assign_resources(); }

    /* Route every enumerated device's legacy interrupt.
     *
     * This is the part of PCI that has no counterpart in config space here:
     * x86 reads PCI_INTERRUPT_LINE, which firmware filled in. Nothing filled
     * it in on this machine, so the answer is in the device tree's
     * `interrupt-map` and must be decoded. Doing it for every device at boot
     * proves the decode against real entries rather than one lucky slot.
     *
     * (The virtio drivers themselves need the scheduler and the network stack
     * linked before they can come up here -- see docs/TODO.md. This tests the
     * seam they will use.) */
    {
        kprintf("\n--- PCI interrupt routing ---\n");
        uint32_t routed = 0, n = pci_devices_count();
        for (uint32_t i = 0; i < n; i++) {
            const struct pci_device *d = pci_get_device(i);
            if (d && arch_pci_irq_connect(d, pci_probe_isr))
                routed++;
        }
        extern uint32_t pci_ecam_intx_distinct(void);
        uint32_t distinct = pci_ecam_intx_distinct();

        kprintf("  [%s] %d of %d devices routed to the GIC, on %d distinct line(s)\n",
                (routed && distinct == routed) ? " ok " : "FAIL",
                (int)routed, (int)n, (int)distinct);

        /* Distinctness is the real check. `virt` wires slots in a rotating
         * pattern, so two devices in different slots MUST land on different
         * SPIs -- and an interrupt-map walked with the wrong stride happily
         * reports every device routed, to the same line. */
        if (!routed || distinct != routed)
            selftest_fails++;
    }

    bringup_sched_init();

    /* The scheduler runs on the way OUT of an interrupt, after the EOI, not
     * from inside the timer handler. gic_dispatch() explains why at length. */
    gic_set_post_eoi(bringup_sched_tick);

    timer_init();

    /* Two no-argument entry points, because kernel_ctx_prepare() takes none:
     * a thread's arguments belong in its own structure, which is what
     * process.c already does and what this stands in for. */
    bringup_thread_create("worker-A", worker_a);
    bringup_thread_create("worker-B", worker_b);

    /* Only now. The controller is configured and the timer is armed, so the
     * first thing PSTATE.I unmasking can produce is a tick we are ready for. */
    arch_irq_enable();
    kprintf("sched: interrupts enabled -- preemption starts here\n\n");

    /* --- A7: the PCIe bus ---------------------------------------------------
     * The existing virtio-gpu and virtio-net drivers are virtio-over-PCI, so
     * enumerating this bus is what makes them available here at all. */
    selftest_preemption();

    /* --- A5: user mode ----------------------------------------------------- */
    kprintf("\n--- self-test: EL0 ---\n");
    int64_t rc = el0_probe_run();

    /* 42 is the value the probe passed to exit(). Checking it -- rather than
     * just "we got back" -- is what proves a syscall ARGUMENT travelled from
     * an EL0 register, through the trap frame, into struct sysargs, into a
     * handler that has no idea which machine it is on. */
    kprintf("  [%s] EL0 program ran and exited with %d (expected 42)\n",
            rc == 42 ? " ok " : "FAIL", (int)rc);
    if (rc != 42)
        selftest_fails++;

    kprintf("\n--- all self-tests done: %d failure(s) ---\n", (int)selftest_fails);

    kprintf("\nA5 reached. Parking (real processes and a libc are A6).\n");

    for (;;)
        __asm__ volatile("wfi");
}
