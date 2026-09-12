#include <stdint.h>
#include "arch/aarch64/drivers/pl011.h"
#include "arch/aarch64/irq/exception.h"
#include "arch/aarch64/boot/fdt.h"
#include "arch/aarch64/mm/pagetable.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/irq/its.h"
#include "arch/aarch64/sched/bringup.h"
#include "drivers/timer/timer.h"
#include "drivers/bus/pci.h"
#include "drivers/storage/virtio_blk.h"
#include "block/block.h"
#include "fs/embkfs/embkfs.h"
#include "mm/swap.h"          /* swap_init: the store, if a disk carries the header */
#include "mm/swaptest.h"
#include "loader/pietest.h"
#include "drivers/storage/nvme.h"
#include "fs/epfs.h"      /* the swap witness, driven */
#include "fs/vfs.h"
#include "arch/aarch64/cpu/cpu_features.h"
#include "include/uaccess_guard.h"
#include "lib/random.h"
#include "lib/canary.h"
#include "drivers/timer/rtc.h"   /* rtc_now_unix: the clock cross-check */
#include "mm/vm_object.h"
#include "power/power.h"
#include "kworker/kworker.h"
#include "process/process.h"
#include "include/errno.h"
#include "boot/boot_protocol.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "mm/vmm.h"
#include "drivers/video/gpu.h"
#include "drivers/video/framebuffer.h"
#include "drivers/video/console.h"
#include "gfx/compositor.h"
#include "drivers/input/virtio_input.h"
#include "drivers/input/mouse.h"
#include "drivers/input/keyboard.h"
#include "drivers/audio/ac97.h"
#include "drivers/audio/audio.h"
#include "include/uaccess_guard.h"
#include "include/arch_ipi.h"
#include "mm/vma.h"
#include "process/futex.h"
#include "process/sched.h"   /* the deadline policy A/B */
#include "arch/aarch64/smp/smp.h"
#include "include/arch_irq.h"
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

/* How long the boot self-test waits for the desktop session to come up and for
 * injected input to arrive, at the 100 Hz tick. Generous on purpose: it is a
 * DEADLINE, not a delay -- the loop leaves the moment its conditions hold, so a
 * healthy system never spends it, and a broken one fails rather than hangs. */
#define DESKTOP_DEADLINE_TICKS 2500

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
static uint64_t selftest_rtc_t0;   /* PL031 seconds when the clocks were first trusted */

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
    uint64_t free_before = pmm_free_pages();
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

    /* The number is the point: destroying an address space walks its tables
     * and frees both the page-table pages and the frames they point at. A walk
     * that misses a level comes back short and says so, where "destroyed"
     * would read as success. */
    uint64_t after = pmm_free_pages();
    bool clean = (after == free_before);
    kprintf("  [%s] both destroyed -- free pages %d -> %d%s\n",
            clean ? " ok " : "FAIL", (int)free_before, (int)after,
            clean ? " (all reclaimed)" : " *** LEAKED ***");
    if (!clean)
        selftest_fails++;
}

/* Read, write, read back. The write matters: a read-only test passes against a
 * driver that returns the bounce buffer's previous contents, and this one
 * cannot -- the pattern has to survive a round trip through the device. */
static void selftest_blk(void) {
    struct embk_block_device *d = embk_block_get(0);
    if (!d) { kprintf("  [FAIL] no block device registered\n"); selftest_fails++; return; }

    static uint8_t buf[512], back[512];
    const uint64_t LBA = 4;          /* past anything a filesystem header uses */

    if (embk_block_read(d, LBA, 1, buf) != EMBK_OK) {
        kprintf("  [FAIL] read of LBA %d failed\n", (int)LBA); selftest_fails++; return;
    }

    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)(0xA7 ^ (i & 0xFF));

    if (embk_block_write(d, LBA, 1, buf) != EMBK_OK) {
        kprintf("  [FAIL] write of LBA %d failed\n", (int)LBA); selftest_fails++; return;
    }
    if (embk_block_read(d, LBA, 1, back) != EMBK_OK) {
        kprintf("  [FAIL] read-back of LBA %d failed\n", (int)LBA); selftest_fails++; return;
    }
    for (int i = 0; i < 512; i++) {
        if (back[i] != buf[i]) {
            kprintf("  [FAIL] LBA %d byte %d: wrote %x read %x\n",
                    (int)LBA, i, buf[i], back[i]);
            selftest_fails++; return;
        }
    }
    kprintf("  [ ok ] %s: 512 bytes written and read back byte-for-byte\n", d->name);
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
    /* What this core can enforce, and turning it on -- before any user process
     * exists, which is the only moment at which "the kernel has never been
     * able to touch user memory" is trivially true. */
    arm_cpu_features_detect();
    arm_protection_init_this_cpu();

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
        uint32_t seen[16] = {0};
        for (uint32_t i = 0; i < n; i++) {
            const struct pci_device *d = pci_get_device(i);
            uint32_t intid = d ? arch_pci_irq_line(d) : 0;
            if (intid) { seen[routed] = intid; routed++; }
        }
        uint32_t distinct = 0;
        for (uint32_t a = 0; a < routed; a++) {
            bool dup = false;
            for (uint32_t b = 0; b < a; b++) if (seen[b] == seen[a]) dup = true;
            if (!dup) distinct++;
        }

        /* Distinctness is the real check. `virt` wires slots in a ROTATING
         * pattern, so devices in different slots must land on different SPIs
         * -- and an interrupt-map walked with the wrong stride happily reports
         * every device routed, to the same line. That is the bug this caught
         * once already (A7's "a bug that looked like success").
         *
         * The rotation is over the FOUR INTx pins, so the expected count is
         * min(routed, 4), not `routed`. It was written as `distinct == routed`
         * when this machine had two PCI devices, and stayed true at three --
         * then virtio-gpu, virtio-keyboard and virtio-tablet arrived and five
         * routed devices on four lines was reported as a FAILURE. The
         * assertion had outgrown its assumption, not the code: five distinct
         * lines are not available to be wrong about. */
        uint32_t expect = routed < 4 ? routed : 4;

        kprintf("  [%s] %d of %d devices routed to the GIC, on %d distinct "
                "line(s) of %d expected\n",
                (routed && distinct == expect) ? " ok " : "FAIL",
                (int)routed, (int)n, (int)distinct, (int)expect);

        if (!routed || distinct != expect)
            selftest_fails++;
    }

    /* A disk. `virt` has neither ATA nor AHCI, so this is the only path to
     * one -- and without a disk there is no filesystem and nothing to run. */
    kprintf("\n--- storage ---\n");
    if (virtio_blk_init())
        selftest_blk();
    else
        kprintf("  [info] no virtio-blk attached\n");
    /* NVMe after virtio-blk, so the root disk the harness attaches first keeps
     * the name sda. On real aarch64 hardware there is no virtio at all and this
     * is the only disk there is. */
    if (nvme_init()) {
        kprintf("  [ ok ] nvme: %d namespace(s) registered\n", nvme_namespace_count());
        /* The same witness x86 runs as `test nvme`. It writes only to a
         * namespace carrying the scratch marker; with none attached it reads
         * block 0 of each namespace and stops. */
        int rc = nvme_selftest_run();
        if (rc == 0)
            kprintf("  [ ok ] nvme: every transfer shape written and read back: OK\n");
        else if (rc != -EMBK_ENOENT) {
            kprintf("  [FAIL] nvme: the witness failed\n");
            selftest_fails++;
        }
    }

    /* --- the real filesystem -------------------------------------------------
     * process_init() first: EMBKFS takes sleeping locks, and a sleeping lock
     * needs a scheduler to sleep on. Then vfs_init(), then embkfs_init(),
     * which probes every registered block device and mounts what it finds --
     * exactly the sequence kernel/main.c runs on x86, against exactly the same
     * code. */
    kprintf("\n--- filesystem ---\n");
    process_init();
    vfs_init();

    /* THE ENDPOINT FILESYSTEM AT /run, registered where kernel/main.c registers
     * it -- right after vfs_init(), before the disk, because IPC rendezvous
     * must not depend on a disk existing.
     *
     * It was simply missing here. epfs.c has always been compiled into this
     * kernel and nothing ever started it, so on aarch64 every chan_listen() and
     * chan_connect() through /run failed: the desktop could not open its
     * launcher channel, and the top bar's request to open the launcher looked
     * up "/run/emlink.desktop" on the ROOT DISK instead -- the log's only
     * trace of it was `"emlink.desktop" not found` on sda. The launcher button
     * did nothing, and had never done anything, on this architecture. The boot
     * test did not notice because nothing in it presses that button; the
     * desktop that comes up looks complete. */
    epfs_init();
    {
        int rc = epfs_vfs_register("/run");
        if (rc != EMBK_OK)
            kprintf("VFS: epfs register at /run failed: %s\n", embk_strerror(rc));
        else
            kprintf("  [ ok ] endpoint filesystem mounted at /run\n");
    }

    embkfs_init();
    swap_init();                 /* a raw device with the EMBKSWAP header, if any -- see mm/swap.h */

    /* embkfs_init() mounts the volume; registering it with the VFS at "/" is a
     * separate step, exactly as kernel/main.c does it -- a mounted volume and a
     * reachable filesystem are two different things. */
    {
        struct embkfs_volume *live = embkfs_live_volume();
        bool ok = false;

        if (live && embkfs_vfs_register("/", live) == EMBK_OK) {
            /* Read a real file. That the superblock parsed proves the disk
             * works; reading a path proves the B-tree walk, the directory
             * lookup, the extent map and the block layer underneath all agree
             * -- on an image built by the x86 toolchain and never touched
             * since. */
            uint8_t hdr[4] = {0};
            size_t got = 0;
            const char *path = "/system/bin/init.elf";
            int rc = vfs_read(path, 0, hdr, sizeof hdr, &got);
            if (rc != EMBK_OK) {
                path = "/system/bin/shell.elf";
                rc = vfs_read(path, 0, hdr, sizeof hdr, &got);
            }

            ok = (rc == EMBK_OK && got == 4 && hdr[0] == 0x7F &&
                  hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F');
            kprintf("  [%s] %s: %d bytes, %s\n", ok ? " ok " : "FAIL",
                    path, (int)got,
                    ok ? "ELF magic intact" : "unreadable or not an ELF");
        } else {
            kprintf("  [FAIL] no EMBKFS volume to register at /\n");
        }
        if (!ok)
            selftest_fails++;
    }

    /* The deferred-teardown worker and the page cache's writeback thread.
     *
     * x86 has started the worker since it existed; aarch64 never did, and the
     * consequence was a real (if quiet) leak -- an exit-time fd close hands its
     * vnode to this thread precisely because obj_put can block on the disk, and
     * with no thread to receive it, nothing released it. The page cache makes
     * that path load-bearing rather than merely leaky: it is also how a dying
     * process's dirty pages reach the device. */
    /* ONE PINNED IDLE THREAD PER CORE -- the liveness backstop process.h calls
     * an invariant, and which aarch64 simply never had. x86 creates these in
     * main.c; nothing here did, so a core with nothing else runnable found NO
     * candidate in schedule_locked's scan and took its early return.
     *
     * That is the path that could leave a thread marked BLOCKED while it was
     * still executing (schedule_locked now unwinds it, which is the real fix)
     * -- but a core that has somewhere to switch TO never reaches it in the
     * first place. Both halves are worth having: one makes the state
     * impossible, the other makes it unreachable. */
    for (uint32_t ci = 0; ci < cpu_count; ci++) {
        if (!process_create_idle_for_cpu(ci))
            kprintf("warning: no idle kthread for cpu %u\n", (unsigned)ci);
    }

    /* COM-equivalent receive, interrupt-driven. After the GIC is up (it needs
     * to register an INTID) and after the device tree is parsed (it needs to
     * find which one). The polled path dropped anything longer than the FIFO
     * pasted between two ticks. */
    pl011_irq_enable();

    kworker_init();
    vmo_writeback_init();
    power_init();

    bringup_sched_init();

    /* The scheduler runs on the way OUT of an interrupt, after the EOI, not
     * from inside the timer handler. gic_dispatch() explains why at length. */
    gic_set_post_eoi(bringup_sched_tick);

    timer_init();

    /* Seed the kernel CSPRNG now that the counter frequency is known --
     * time_get_ns() is 0 before timer_init(), and a seed that mixes a clock
     * that does not move is the mistake this is placed to avoid. Same rule as
     * x86 (after tsc_calibrate). Before any user process is spawned, because
     * each one's address-space layout is drawn from this. */
    random_init();

    /* A SECOND OPINION ON THE CLOCK. Every millisecond this kernel reports on
     * aarch64 -- scheduler lateness, audio runway, the self-test budget -- is
     * CNTVCT divided by CNTFRQ, and a hypervisor that virtualises the counter
     * at one rate while advertising another would make all of them wrong by
     * the same factor, invisibly: uptime and the tick count are derived from
     * the same registers and would agree with each other perfectly. The PL031
     * is a different device tracking host wall time. Sampled here and again at
     * the final tally, the two deltas either agree or they do not. */
    selftest_rtc_t0 = rtc_now_unix();

    /* Two no-argument entry points, because kernel_ctx_prepare() takes none:
     * a thread's arguments belong in its own structure, which is what
     * process.c already does and what this stands in for. */
    bringup_thread_create("worker-A", worker_a);
    bringup_thread_create("worker-B", worker_b);

    /* Only now. The controller is configured and the timer is armed, so the
     * first thing PSTATE.I unmasking can produce is a tick we are ready for. */
    arch_irq_enable();
    kprintf("sched: interrupts enabled -- preemption starts here\n\n");

    /* The boot core's own SGIs, before any secondary exists to send it one. */
    arch_ipi_init_this_cpu();

    /* The ITS: MSI. After the GIC (it targets a redistributor) and before the
     * PCI drivers that might want one. Absence is not a failure -- every
     * driver falls back to the legacy INTx line this machine also provides. */
    /* Interrupts are already unmasked at this point in the boot, so the ITS
     * can prove itself immediately: a delivery it cannot observe is not a
     * proof. */
    if (its_init() && !its_selftest())
        selftest_fails++;

    /* --- A7: the PCIe bus ---------------------------------------------------
     * The existing virtio-gpu and virtio-net drivers are virtio-over-PCI, so
     * enumerating this bus is what makes them available here at all. */
    selftest_preemption();

    /* --- A6: a REAL user process ---------------------------------------------
     *
     * The hand-written EL0 probe that used to sit here is gone. It proved the
     * `eret`/`svc` transition against a hand-mapped address space and a
     * four-entry syscall table; what runs now is a newlib program off the
     * filesystem, through process_create(), into the real 95-handler table.
     * docs/TODO.md scheduled the probe's deletion at exactly this point.
     *
     * THREE THINGS HAVE TO HAPPEN IN THIS ORDER, and each is a different kind
     * of handover:
     *
     * 1. The boot context becomes a THREAD. schedule() returns immediately if
     *    current_thread is NULL, so without this the timer would tick, the
     *    scheduler would be called, and it would decline to do anything -- a
     *    silent failure that looks exactly like a process that never runs.
     *    process_adopt_current() is the same call main.c makes on x86.
     *
     * 2. The tick starts driving the REAL scheduler. Up to here the GIC's
     *    post-EOI hook ran bringup_sched_tick(), the stand-in that owns the A3
     *    preemption demo above. That demo is finished; from now on the hook is
     *    process.c's schedule(), which is what x86's lapic_timer_handler()
     *    calls at the same point in the same order (after the EOI, never from
     *    inside the handler -- gicv3.c explains why at length).
     *
     * 3. Only THEN is the process created. process_create() marks it RUNNABLE
     *    immediately, so doing it before step 2 would leave a runnable process
     *    that nothing would ever pick up.
     *
     * hello.elf rather than init.elf, and that is an honest statement of where
     * the port is. init is the desktop's supervisor: it spawns login, then the
     * session, then home.elf, none of which exist for aarch64 until libembk.so
     * and the compositor land (A7). hello.elf is a real newlib program --
     * printf through _write, malloc through _sbrk, time() through
     * _gettimeofday, and a native EmbLink thread -- and it EXITS WITH THE
     * NUMBER OF CHECKS THAT PASSED, which is what makes it a test rather than
     * a demo. */
    /* --- A7: the display -----------------------------------------------------
     * gpu_init() picks a driver; on this machine virtio_gpu_probe() is the only
     * one that can answer, because absent.c's bochs_gpu_probe() returns NULL and
     * `virt` has no VGA aperture for it to have found anyway (ARM64.md §6.5).
     * Then the SHARED framebuffer and text console come up on top of it, exactly
     * as kernel/main.c orders them: a driver, then a surface, then something
     * that draws on it.
     *
     * After PCI, and that is not arbitrary -- virtio-gpu is a PCI device whose
     * BARs this kernel assigned itself a few hundred lines up, because there is
     * no firmware here to have done it. */
    kprintf("\n--- display (A7) ---\n");
    gpu_init();
    fb_init();
    console_init();
    {
        const fb_info_t *fbi = fb_get_info();
        if (fbi && fbi->width && fbi->height) {
            kprintf("  [ ok ] framebuffer %ux%u, %u bpp\n",
                    (unsigned)fbi->width, (unsigned)fbi->height,
                    (unsigned)fbi->bpp);
        } else {
            kprintf("  [info] no display device attached\n");
        }

        /* Input. virtio-input replaces the PS/2 keyboard and mouse that
         * absent.c used to answer for -- its stubs are deleted now, so these
         * are the real shared drivers.
         *
         * mouse_init() AFTER the framebuffer, and with its real size: it sets
         * the cursor's clamp bounds, and clamping to a guessed 1024x768 on a
         * 1280x800 screen leaves a band the pointer can never reach. Same
         * ordering, same reason, as kernel/main.c. */
        mouse_init(fbi ? fbi->width : 1024, fbi ? fbi->height : 768);
        virtio_input_init();

        /* Sound. Named ac97_init() because that is what the shared audio layer
         * calls; what answers here is virtio_snd.c. Harmless with no device
         * attached -- it says so and audio_available() stays false. */
        ac97_init();

        /* AUDIO, PROVEN RATHER THAN PROBED. "stream 0 ready" means the device
         * accepted SET_PARAMS and PREPARE; it does not mean a single sample
         * ever reached it. So push a real buffer through the SHARED audio
         * layer -- the same audio_open/audio_write/audio_drained path a
         * userland program uses -- and wait for the device to hand it back.
         * A buffer that is submitted and never retired is the failure this
         * catches, and it is invisible from the setup handshake. */
        if (audio_available()) {
            static int16_t tone[512 * 2];
            for (int i = 0; i < 512; i++) {
                /* A square wave: no math library in the kernel, and the point
                 * is that bytes move, not that they sound pleasant. */
                int16_t v = (i / 32) % 2 ? 6000 : -6000;
                tone[i * 2] = tone[i * 2 + 1] = v;
            }

            uint32_t acc = 0;
            int rc = audio_open(0);
            if (rc == EMBK_OK)
                rc = audio_write(0, tone, 512, &acc);

            bool drained = false;
            if (rc == EMBK_OK && acc > 0) {
                uint64_t start = timer_sched_ticks();
                while (timer_sched_ticks() < start + 200) {
                    if (audio_drained(0)) { drained = true; break; }
                    arch_cpu_idle();
                }
            }
            audio_close(0);

            kprintf("  [%s] audio: %u frame(s) accepted at %u Hz, buffer%s retired\n",
                    (rc == EMBK_OK && acc > 0 && drained) ? " ok " : "FAIL",
                    (unsigned)acc, (unsigned)audio_sample_rate(),
                    drained ? "" : " NOT");
            if (rc != EMBK_OK || acc == 0 || !drained)
                selftest_fails++;
        } else {
            kprintf("  [info] no audio device attached\n");
        }
    }

    /* --- A9: the other cores -------------------------------------------------
     * After the GIC, the timer and the scheduler, and before userland: a
     * secondary needs all three to exist before it can join, and the work it
     * then picks up is user work. */
    smp_bringup();

    /* --- cross-core interrupts ----------------------------------------------
     * "The SGI was sent" and "another core ran the handler" are different
     * claims, and only the second one is worth making: a broadcast that
     * reaches nobody looks exactly like a working one from here. So the count
     * comes from the RECEIVING cores -- ipi_dispatch() increments it, and it
     * is shared memory, so a non-zero delta means somebody else really did
     * enter the handler.
     *
     * IPI_RESCHEDULE is the one to test with: its handler does nothing at all
     * (the interrupt IS the point -- it drags a core into the interrupt path,
     * whose exit already runs the scheduler), so counting it disturbs nothing. */
    if (cpu_count > 1) {
        uint64_t before = ipi_count(IPI_RESCHEDULE);
        arch_ipi_broadcast(IPI_RESCHEDULE);

        uint64_t deadline = timer_sched_ticks() + 100;
        while (ipi_count(IPI_RESCHEDULE) < before + (cpu_count - 1) &&
               timer_sched_ticks() < deadline)
            arch_cpu_relax();

        uint64_t got = ipi_count(IPI_RESCHEDULE) - before;
        bool ok = got >= cpu_count - 1;
        kprintf("  [%s] IPI: %d of %d other core(s) took the interrupt\n",
                ok ? " ok " : "FAIL", (int)got, (int)(cpu_count - 1));
        if (!ok)
            selftest_fails++;
    }

    kprintf("\n--- userland (A6) ---\n");
    {
        struct thread *self = process_adopt_current();
        if (!self) {
            kprintf("  [FAIL] could not adopt the boot context as a thread\n");
            selftest_fails++;
        } else {
            gic_set_post_eoi(schedule);

    /* --- the user-copy recovery guard ---------------------------------------
     * Proven by CAUSING the fault it exists for. access_ok() cannot be made to
     * pass on an unmapped page, so the race it protects against cannot be
     * staged from here honestly -- but the RECOVERY can: arm the guard, touch
     * an address that is guaranteed to fault at EL1, and check we come back
     * with an error instead of a panic.
     *
     * That is the whole claim. Without the guard this exact sequence is an
     * unhandled kernel data abort, which is what a user program racing its own
     * threads could provoke on purpose.
     *
     * AFTER process_adopt_current(), and that is not incidental: the recovery
     * point lives in the THREAD (a copy is preemptible, so a per-CPU slot
     * would be the wrong one after a migration). With no current thread the
     * guard correctly declines to arm -- and nothing can race a kernel that
     * has no processes yet, which is why declining is right rather than a
     * hole. Run before the adopt, this test armed nothing and panicked. */
    kprintf("\n--- user-copy fault recovery ---\n");
    {
        uint64_t before = uaccess_recoveries();
        volatile int sink = 0;
        bool recovered = false;

        if (uaccess_arm()) {
            /* A user-half address with nothing mapped at it. The kernel may
             * read the user half (PAN is not enabled here), so this reaches
             * the page tables and finds nothing -- a translation fault at
             * EL1, which is precisely the shape of the race. */
            sink = *(volatile int *)(uintptr_t)0x0000700000ABC000ULL;
            uaccess_disarm();
        } else {
            recovered = true;
        }
        (void)sink;

        bool ok = recovered && uaccess_recoveries() == before + 1;
        kprintf("  [%s] a kernel fault on user memory returned an error "
                "instead of panicking (%d recovery/ies)\n",
                ok ? " ok " : "FAIL", (int)uaccess_recoveries());
        if (!ok)
            selftest_fails++;
    }


            char *hargv[] = { (char *)"/system/bin/hello.elf", NULL };
            int pid = process_create("/system/bin/hello.elf", hargv, 1, NULL, 0);
            if (pid < 0) {
                kprintf("  [FAIL] could not launch /system/bin/hello.elf: %s\n",
                        embk_strerror(pid));
                selftest_fails++;
            } else {
                kprintf("  [ ok ] /system/bin/hello.elf launched as pid %d\n", pid);

                /* Wait for it, rather than falling into the idle loop below.
                 * The exit code IS the assertion: hello.c counts the checks it
                 * passed and returns that count, so a number here says which
                 * parts of the retargeting layer work, and the acceptance test
                 * greps for it. */
                int code = process_wait(pid);
                kprintf("  [%s] hello.elf exited with %d (checks passed)\n",
                        code > 0 ? " ok " : "FAIL", code);
                if (code <= 0)
                    selftest_fails++;
            }
        }
    }

    /* --- A6 + A7: a dynamically-linked EmUI app, and a frame on the screen ----
     *
     * This is BOTH phases' "done when" in one run, and they are one test
     * because neither half means anything alone: A6 asks for a dynamically
     * linked app to load, A7 asks for the compositor to present a frame on
     * `virt`, and an app with no display would have nothing to present while a
     * display with no app would have nothing to show.
     *
     * WHAT IS SPAWNED IS init.elf, the same pid 1 x86 starts, not a demo app.
     * init is the root of userspace authority (docs/USERSPACE_v2.md UP1): it
     * brings up the session and supervises it, so the DESKTOP is init's child
     * rather than the first process. It reaches /system/bin/home.elf, which is
     * an ET_EXEC with PT_DYNAMIC and DT_NEEDED libembk.so -- so this one spawn
     * exercises the freestanding loader, the dynamic loader, and the two-way
     * link between them: the app's imports resolve to the toolkit's exports,
     * and the toolkit's libc imports (malloc, memcpy, sinf) resolve BACK into
     * the app, where newlib was statically pulled in. Every one of those
     * relocations is applied by code that needed no aarch64-specific line --
     * elf.c has been written against the neutral ELF_RELOC_* names since A4.
     *
     * The boot thread then pumps the compositor exactly as main.c's boot loop
     * does on x86, and for the same reason stated there: these repaint, so they
     * must run in schedulable context, never from an IRQ handler. */
    /* The POSIX conformance witness, and specifically the TLS half of it.
     * hello.elf above proves the retargeting layer works; posixdemo proves it
     * is RIGHT -- 100-odd assertions over stdio, dirents, paths, clocks,
     * signals and thread-local storage, exiting 0 only if every one passes.
     *
     * The TLS block is the part worth running HERE rather than trusting: it
     * asserts variant I -- thread pointer in TPIDR_EL0, TLS variables ABOVE it,
     * past a 16-byte TCB -- which is precisely what crt0.c had to implement
     * differently from x86 and what nothing else checks. Build it on the wrong
     * side of the thread pointer and there is no fault, just every
     * thread-local silently resolving into the TCB. */
    {
        const char *pd = "/system/bin/posixdemo.elf";
        char *pargv[] = { (char *)pd, NULL };
        int ppid = process_create(pd, pargv, 1, NULL, 0);
        if (ppid < 0) {
            kprintf("  [FAIL] could not launch %s: %s\n", pd, embk_strerror(ppid));
            selftest_fails++;
        } else {
            int code = process_wait(ppid);
            kprintf("  [%s] posixdemo exited %d (0 == every assertion passed)\n",
                    code == 0 ? " ok " : "FAIL", code);
            if (code != 0)
                selftest_fails++;
        }
    }

    /* --- the futex, and the userland mutex on it ---------------------------
     * Threads existed here long before any way for two of them to agree about
     * anything. The claim is not "a lock exists" -- it is that four threads
     * hammering one counter produce the EXACT total, which is the only thing a
     * broken mutex gets wrong (it loses updates; it does not crash). */
    /* --- swap, on THIS architecture too --------------------------------------
     * The same witness x86 runs as `make test-swap`: more anonymous memory
     * than the machine has free, written and read back, mmap then malloc, and
     * the kernel's counters saying the pages went out to the store (a third
     * virtio-blk, "sdc") and came back. The pager, the objects and the store
     * are shared code; the disk under them is not. Quick mode: the hot-set
     * A/B is a policy claim, made once, on x86. */
    /* --- the account store, on this architecture too -----------------------
     * user/tests/authtest/authtest.c against a scratch store: every operation the greeter
     * offers, the upgrade of a standard PBKDF2 record made elsewhere, and the
     * clamp on session profiles. At native speed here, which is where the
     * work factor's real cost can be read off. */
    kprintf("\n--- accounts ---\n");
    {
        const char *tp = "/data/apps/authtest/authtest.elf";
        char *a[] = { (char *)tp, NULL };
        int pid = process_create(tp, a, 1, NULL, 0);
        int rc = pid >= 0 ? process_wait((uint32_t)pid) : pid;
        kprintf("  [%s] the account store and the session policy: %s (exit %d)\n",
                rc == 0 ? " ok " : "FAIL", rc == 0 ? "OK" : "FAIL", rc);
        if (rc != 0) selftest_fails++;
    }

    /* --- a position-independent executable, on this architecture too -------
     * The aarch64 relocation types and the aarch64 linker are the only parts
     * of PIE that differ from x86, and they are exactly the parts a shared
     * loader cannot prove on one machine. Same witness, same claim: a real
     * ET_DYN binary loads twice and lands somewhere different each time. */
    kprintf("\n--- pie ---\n");
    {
        int rc = pie_selftest_run("/data/apps/pieprobe/pieprobe.elf");
        if (rc == -EMBK_ENOENT) {
            kprintf("  [ -- ] position-independent executables: NOT BUILT (no -fPIC libc)\n");
        } else {
            kprintf("  [%s] a PIE moves between runs: %s\n",
                    rc == 0 ? " ok " : "FAIL", rc == 0 ? "OK" : "FAIL");
            if (rc != 0) selftest_fails++;
        }
    }

    kprintf("\n--- swap ---\n");
    {
        int rc = swap_selftest_run("/data/apps/swapper/swapper.elf", true);
        kprintf("  [%s] anonymous memory larger than RAM: %s\n",
                rc == 0 ? " ok " : "FAIL", rc == 0 ? "OK" : "FAIL");
        if (rc != 0) selftest_fails++;
    }

    kprintf("\n--- futex ---\n");
    {
        const char *lp = "/system/bin/lockdemo.elf";
        char *a[] = { (char *)lp, NULL };
        uint64_t w0, k0, e0, w1, k1, e1;
        futex_stats(&w0, &k0, &e0);
        int pid = process_create(lp, a, 1, NULL, 0);
        int rc  = pid >= 0 ? process_wait((uint32_t)pid) : -1;
        futex_stats(&w1, &k1, &e1);
        kprintf("  [%s] 4 threads x 2000 increments: the total is EXACT (exit %d)\n",
                rc == 0 ? " ok " : "FAIL", rc);
        kprintf("  [info] %llu futex waits, %llu wakes, %llu EAGAIN\n",
                (unsigned long long)(w1 - w0), (unsigned long long)(k1 - k0),
                (unsigned long long)(e1 - e0));
        if (rc != 0)
            selftest_fails++;
    }

    /* --- the deadline scheduler policy -------------------------------------
     * The same A/B the x86 console runs as `test deadline`: one binary, one
     * load, two policies. The assertion is the comparison, because "13 ms of
     * jitter" is neither good nor bad on its own -- and it is run here rather
     * than assumed to port, since the whole policy rests on the timer and the
     * timers are the part of the two architectures with nothing in common.
     *
     * Fewer periods than x86 (60, not 150): this runs under TCG as well as
     * HVF, and the boot test has to finish. */
    kprintf("\n--- deadline scheduler ---\n");
    {
        const char *jp = "/system/bin/jitter.elf";
        char *a[] = { (char *)jp, "6", "16", "60", "2", NULL };

        const struct sched_policy *saved = sched_policy_get();
        const struct sched_policy *rr = sched_policy_by_name("round-robin");
        const struct sched_policy *dl = sched_policy_by_name("deadline");

        if (!rr || !dl) {
            kprintf("  [FAIL] a policy is missing from the table\n");
            selftest_fails++;
        } else {
            sched_policy_set(rr);
            int prr = process_create(jp, a, 5, NULL, 0);
            int wrr = prr >= 0 ? process_wait((uint32_t)prr) : -1;

            sched_policy_set(dl);
            int pdl = process_create(jp, a, 5, NULL, 0);

            int wdl = pdl >= 0 ? process_wait((uint32_t)pdl) : -1;

            sched_policy_set(saved);

            /* AT MOST A QUARTER, not merely "no worse". The old assertion was
             * wdl <= wrr, and it let the real failure through: when the policy
             * silently did not apply (the declared-count reap race, fixed in
             * process.c), the deadline run measured 44 ms against round-robin's
             * 44 ms and PASSED. The policy working is not a tie -- it is 0-1 ms
             * against 42-52 here and 4 ms against 115 on x86, so a quarter is a
             * wide margin that still fails the moment the policy is not in
             * effect. */
            bool dl_ok = (wrr >= 0 && wdl >= 0 && wdl * 4 <= wrr);
            kprintf("  [%s] worst lateness: round-robin %d ms, deadline %d ms "
                    "(the policy must be 4x better, not merely no worse)\n",
                    dl_ok ? " ok " : "FAIL", wrr, wdl);
            if (!dl_ok)
                selftest_fails++;
            if (wrr >= 0 && wrr < 10)
                kprintf("  [info] round-robin's worst was only %d ms -- the load\n"
                        "         did not contend, so this is not evidence\n", wrr);
            kprintf("  [%s] no reservation outlived its thread (%u permille)\n",
                    sched_reserved_permille() == 0 ? " ok " : "FAIL",
                    sched_reserved_permille());
            if (sched_reserved_permille() != 0)
                selftest_fails++;
        }
    }

    /* --- mmap / munmap ------------------------------------------------------
     * The claim is not "the syscall returns an address" -- it is that the
     * address WORKS and that unmapping GIVES THE MEMORY BACK. Both are checked
     * against the physical allocator's own free count, which is the only
     * number that cannot be faked by the thing under test. */
    /* --- crash consistency, on THIS architecture too ----------------------
     * The same test x86 runs as `make test-embkfs-crash`: the small seed image
     * (attached by the harness as a second virtio-blk, "sdb") is copied into a
     * RAM-backed block device, a workload of commits runs against it with
     * every write after the N-th dropped, and the survivor must be exactly the
     * state after some whole number of commits -- for every N. The filesystem
     * is shared code; the block driver, the DMA path and the cache flushes
     * under it are not, which is why it runs here and not only there. */
    kprintf("\n--- EMBKFS crash consistency ---\n");
    {
        struct embk_block_device *seed = embk_block_get_by_name("sdb");
        if (!seed) {
            kprintf("  [info] no seed disk (sdb) attached: not run\n");
        } else {
            int rc = embkfs_run_crash_selftests(seed);
            kprintf("  [%s] power cut at every write: %s\n",
                    rc == EMBK_OK ? " ok " : "FAIL", rc == EMBK_OK ? "OK" : embk_strerror(rc));
            if (rc != EMBK_OK) selftest_fails++;
        }
    }

    kprintf("\n--- mmap ---\n");
    {
        struct process *p = current_thread ? current_thread->proc : 0;
        uint64_t free_before = pmm_free_pages();
        const uint64_t LEN = 16 * 4096;

        int64_t a = p ? vma_mmap(p, 0, LEN, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE)
                      : -1;
        bool ok = a > 0;

        if (ok) {
            /* Zeroed on arrival, then writable, then readable -- in that
             * order, because a mapping that reads back what you wrote but
             * arrived full of someone else's data is a disclosure that a
             * write-then-read test cannot see.
             *
             * BRACKETED, because this is EL1 touching a USER mapping and PAN
             * forbids exactly that. It is a deliberate access -- the whole
             * point of the case is to look at the page a user would see -- so
             * it asks, the way every other deliberate user access in this
             * kernel asks. This was the first thing PAN caught when it was
             * turned on, which is a good advertisement for it: the code was
             * always reaching into user memory from privileged context, and
             * nothing had ever said so. */
            uaccess_hw_begin();
            volatile uint64_t *m = (volatile uint64_t *)(uintptr_t)a;
            for (uint64_t i = 0; i < LEN / 8; i += 512)
                if (m[i] != 0) ok = false;
            for (uint64_t i = 0; i < LEN / 8; i += 512)
                m[i] = 0xC0FFEE00ULL + i;
            for (uint64_t i = 0; i < LEN / 8; i += 512)
                if (m[i] != 0xC0FFEE00ULL + i) ok = false;
            uaccess_hw_end();
        }

        /* THE ACCESS FLAG, software-managed here: clearing it drops the TLB
         * entry, and the next touch takes an access-flag fault that the
         * handler resolves by setting it -- the round trip the reclaimer's
         * second chance depends on, and the one path a translation fault
         * never exercises. */
        bool acc1 = false, acc2 = false;
        if (ok && p) {
            acc1 = vmm_test_and_clear_accessed_in(p->pml4_phys, (uint64_t)a);
            uaccess_hw_begin();
            (void)*(volatile uint64_t *)(uintptr_t)a;         /* access-flag fault, resolved */
            uaccess_hw_end();
            acc2 = vmm_test_and_clear_accessed_in(p->pml4_phys, (uint64_t)a);
            kprintf("  [%s] the access flag: set by the touches, clear once read, set again through the fault\n",
                    (acc1 && acc2) ? " ok " : "FAIL");
            if (!(acc1 && acc2)) selftest_fails++;
        }

        uint64_t free_mapped = pmm_free_pages();
        bool took = free_mapped <= free_before - LEN / 4096;

        int rc = (p && a > 0) ? vma_munmap(p, (uint64_t)a, LEN) : -1;
        uint64_t free_after = pmm_free_pages();
        bool gave_back = (rc == EMBK_OK) && (free_after == free_before);

        kprintf("  [%s] %d KiB mapped at %p, zeroed, written and read back\n",
                ok ? " ok " : "FAIL", (int)(LEN / 1024), (void *)(uintptr_t)a);
        kprintf("  [%s] free pages %d -> %d -> %d (munmap returned the memory)\n",
                (took && gave_back) ? " ok " : "FAIL",
                (int)free_before, (int)free_mapped, (int)free_after);

        /* W^X is refused, not granted. */
        int64_t wx = p ? vma_mmap(p, 0, 4096, PROT_WRITE | PROT_EXEC,
                                  MAP_ANONYMOUS | MAP_PRIVATE) : 0;
        kprintf("  [%s] a writable+executable mapping was refused\n",
                wx < 0 ? " ok " : "FAIL");

        /* --- AND THAT PAN IS ACTUALLY ENFORCING -----------------------------
         *
         * "SCTLR_EL1.SPAN is clear" and "EL1 cannot read a user page" are two
         * different claims and only the second is worth anything. There is a
         * live user mapping right here, so the violation is committed on
         * purpose: read it WITHOUT asking, under the recovery guard, and
         * require a fault. On a core without FEAT_PAN the read succeeds and
         * this says so -- that is the honest outcome for hardware that cannot
         * refuse it, not a failure. */
        if (p && a > 0) {
            const struct arm_cpu_features *cf = arm_cpu_features();
            uint64_t before = uaccess_recoveries();
            volatile uint64_t seen = 0;
            int faulted = 0;

            if (uaccess_arm()) {
                seen = *(volatile uint64_t *)(uintptr_t)a;   /* no hw_begin */
                uaccess_disarm();
            } else {
                faulted = 1;
            }
            uint64_t caught = uaccess_recoveries() - before;

            if (cf->pan) {
                kprintf("  [%s] PAN: EL1 reading a USER page without asking "
                        "FAULTS (PSTATE.PAN=%d, caught %d)\n",
                        (faulted && caught == 1) ? " ok " : "FAIL",
                        (int)arm_read_pan(), (int)caught);
                if (!faulted || caught != 1) selftest_fails++;
            } else {
                kprintf("  [info] no FEAT_PAN on this core; the unguarded read "
                        "returned 0x%llx. PXN still holds.\n",
                        (unsigned long long)seen);
            }
        }

        if (!ok || !took || !gave_back || wx >= 0)
            selftest_fails++;
    }

    /* --- the stack canary: seeded, and it FIRES ------------------------------
     * Same test as x86's `test canary`: the guard is not the compile-time
     * initialiser (so boot.S seeded it), and a deliberate 8-byte overrun of a
     * 16-byte local reaches __stack_chk_fail, which lands back here through
     * the armed recovery point instead of halting. */
    kprintf("\n--- the stack canary ---\n");
    {
        uintptr_t g = canary_value();
        bool seeded = g && g != 0x5A6B7C8D9EAFB0C1ULL;
        kprintf("  [%s] guard 0x%lx is seeded (not the initialiser)\n",
                seeded ? " ok " : "FAIL", (unsigned long)g);
        if (!seeded) selftest_fails++;

        uint64_t before = canary_fires();
        int landed = 0;
        if (canary_test_arm()) {
            canary_smash_a_frame(24);
            canary_test_disarm();
            kprintf("  [FAIL] the smashed frame returned -- the protector did not fire\n");
            selftest_fails++;
        } else {
            landed = 1;
        }
        uint64_t fired = canary_fires() - before;
        kprintf("  [%s] canary: a deliberate overrun was caught (fired %d, recovered %d)\n",
                (landed && fired == 1) ? " ok " : "FAIL", (int)fired, landed);
        if (!landed || fired != 1) selftest_fails++;
    }

    kprintf("\n--- the desktop (A6 + A7) ---\n");
    {
        const char *app = "/system/bin/init.elf";
        char *uargv[] = { (char *)app, NULL };

        /* Hand the screen to userspace BEFORE the app becomes schedulable --
         * the same call, in the same position, for the same reason main.c
         * documents on x86: process_create() makes the app runnable
         * immediately, and a timer preemption in the gap between the spawn and
         * this call would let its first frame land on top of a boot log that
         * is still painting. Serial keeps every line either way, which is
         * where the acceptance test reads them from. */
        console_set_fb_enabled(false);

        int upid = process_create(app, uargv, 1, NULL, 0);

        if (upid < 0) {
            kprintf("  [FAIL] could not launch %s: %s\n", app,
                    embk_strerror(upid));
            selftest_fails++;
        } else {
            kprintf("  [ ok ] %s launched as pid %d\n", app, upid);

            /* Bounded, not forever: this is a boot self-test, and a test that
             * hangs when the thing it checks is broken reports nothing. Five
             * seconds at 100 Hz is far more than the app needs -- on x86 the
             * equivalent app presents its first frame about 900 ms in, and most
             * of that is parsing the font. */
            /* Pump until the session is UP and input has ARRIVED, with a
             * deadline -- not for a fixed number of ticks.
             *
             * A fixed window was wrong twice over. Too short and the test fails
             * on a working system: the session got slower the moment the image
             * grew a 20 MB wallpaper, because virtio-blk is polled. Too long and
             * every passing run pays the worst case. Waiting for the CONDITION
             * costs what it costs and no more, and the deadline is what keeps a
             * broken system from hanging the test instead of failing it. */
            uint64_t start = timer_sched_ticks();
            kprintf("  [info] waiting for the session (deadline %u ticks)\n",
                    (unsigned)DESKTOP_DEADLINE_TICKS);
            while (timer_sched_ticks() < start + DESKTOP_DEADLINE_TICKS) {
                /* virtio-input is POLLED, and this is its cadence: the same
                 * schedulable context the compositor ticks run in, never an
                 * IRQ handler. main.c's boot loop drives usb_poll() from the
                 * identical place for the identical reason. */
                virtio_input_poll();
                compositor_pointer_tick();
                compositor_anim_tick();

                uint32_t k = 0, pt = 0;
                virtio_input_stats(&k, &pt);
                if (compositor_focused_pid() != 0 && k > 0 && pt > 0)
                    break;          /* everything this phase asserts is true */

                /* DIAGNOSTIC: a session that has not appeared after 800 ticks
                 * is stuck somewhere; say where. Each of init's threads: its
                 * state, the PC and SP its kernel context was saved with, what
                 * it waits on, and how deep in the kernel it is. */
                static bool dumped = false;
                if (!dumped && timer_sched_ticks() > start + 800) {
                    dumped = true;
                    sched_lock();
                    struct process *ip = process_find((uint32_t)upid);
                    kprintf("  [diag] init pid %d: %s\n", upid, ip ? "found" : "GONE");
                    for (struct thread *t = ip ? ip->thread_list : NULL; t; t = t->proc_thread_next) {
                        kprintf("  [diag]   thread %p: state %d cpu %d in_kernel %d killed %d wq %p ctx.pc %llx ctx.sp %llx\n",
                                (void *)t, (int)t->state, (int)t->running_cpu,
                                (int)t->in_kernel, (int)t->killed, (void *)t->wait_queue,
                                (unsigned long long)t->ctx.pc, (unsigned long long)t->ctx.sp);
                    }
                    sched_unlock();
                }

                arch_cpu_idle();
            }
            kprintf("  [info] waited %u tick(s)\n",
                    (unsigned)(timer_sched_ticks() - start));

            /* The assertion. A window that exists AND holds focus means
             * something got through the toolkit, asked the compositor for a
             * surface, and was accepted -- which only happens once there is a
             * framebuffer under it.
             *
             * The focused pid is NOT init's: init spawns the session and the
             * session owns the window, so what this checks is "some process
             * holds focus", not "the process I started does". Asserting
             * equality here would be asserting that init never delegated,
             * which is the opposite of what it is for. */
            uint32_t focused = compositor_focused_pid();
            bool ok = (focused != 0);
            kprintf("  [%s] compositor: focused pid %u (init is %d)\n",
                    ok ? " ok " : "FAIL", (unsigned)focused, upid);
            if (!ok)
                selftest_fails++;

            /* INPUT ACTUALLY ARRIVED, which is a different claim from "the
             * device enumerated". The acceptance test drives QEMU's monitor
             * during the window above -- sendkey, then mouse_move -- so these
             * counters are non-zero only if events crossed the queue, were
             * decoded, and reached the shared keyboard/mouse state. Run by
             * hand with no monitor driving it, they are legitimately zero;
             * that is why this reports rather than fails. */
            uint32_t keys = 0, ptr = 0;
            virtio_input_stats(&keys, &ptr);
            int32_t mx = 0, my = 0; uint32_t mb = 0;
            mouse_get_state(&mx, &my, &mb);
            kprintf("  [info] virtio-input: %u key event(s), %u pointer "
                    "event(s), cursor at %d,%d\n",
                    (unsigned)keys, (unsigned)ptr, (int)mx, (int)my);
        }
    }

    /* Sampled HERE, seconds after bring-up, because "a core takes interrupts"
     * is a rate and not an event: the same counts read immediately after
     * smp_bringup() are near zero on every core simply because no time has
     * passed. Every core must be TICKING -- a core whose timer fired once and
     * stopped is a core that will never preempt anything it is given. */
    if (cpu_count > 1) {
        bool all_ticking = true;
        for (uint32_t c = 0; c < cpu_count; c++) {
            uint64_t n = gic_percpu_irq_count(c);
            kprintf("  cpu%d: %d interrupt(s)\n", (int)c, (int)n);
            if (n < 10)
                all_ticking = false;
        }
        kprintf("  [%s] every core is taking interrupts on its own timer\n",
                all_ticking ? " ok " : "FAIL");
        if (!all_ticking)
            selftest_fails++;
    }

    /* WITH THE CLOCK, because the harness cannot see it. The per-accelerator
     * budget in arch.mk is a fixed sleep-then-kill, and every time it has been
     * wrong it was wrong in the same way: a run that ends early reports every
     * later marker as missing, which reads as a dozen unrelated failures. This
     * line is what makes the budget a measurement -- set it from this number
     * plus headroom, not from how long it felt. */
    {
        uint64_t rtc_dt = rtc_now_unix() - selftest_rtc_t0;    /* whole seconds */
        uint64_t up     = timer_uptime_ms();
        kprintf("\n--- all self-tests done: %d failure(s), at %llu ms of uptime; "
                "the RTC saw %llu s pass since the clocks came up ---\n",
                (int)selftest_fails, (unsigned long long)up,
                (unsigned long long)rtc_dt);
        /* If the two disagree by more than the RTC's one-second granularity
         * plus the boot time before the sample, CNTFRQ is lying and every
         * millisecond above is wrong by the same ratio. Said here, once, in
         * the log, rather than left for someone to notice. */
        if (rtc_dt > 2 && (up / 1000) * 2 < rtc_dt)
            kprintf("  [WARN] guest uptime (%llu s) is far below wall time (%llu s): "
                    "the counter frequency is not what CNTFRQ claims\n",
                    (unsigned long long)(up / 1000), (unsigned long long)rtc_dt);
    }

    kprintf("\nA7 reached: the whole shared kernel is linked and running here.\n");

    /* --- the boot CPU's permanent loop --------------------------------------
     * NOT `wfi` forever, which is what used to be here and was wrong the moment
     * there was a desktop to use: virtio-input is POLLED, so a kernel that
     * stops calling virtio_input_poll() has a keyboard and a mouse that work
     * for exactly as long as the self-test window and then silently stop. The
     * screenshot that caught this had a cursor sitting where the test had left
     * it, ignoring every event QEMU sent afterwards.
     *
     * This is the aarch64 counterpart of main.c's boot loop, with the same
     * three responsibilities and the same reasoning for each: drain the polled
     * input devices, drive the compositor's pointer (cursor, click-to-focus,
     * title-bar drag) and advance window animation -- all in SCHEDULABLE
     * context, never from an IRQ handler, because they repaint and the
     * compositor's lock must never be taken from an interrupt.
     *
     * arch_cpu_idle() between passes rather than a busy spin: the timer tick
     * wakes us at 100 Hz, which is the cadence a cursor needs and a great deal
     * cheaper than spinning. */
    for (;;) {
        virtio_input_poll();
        compositor_pointer_tick();
        /* The system key shortcuts, on this arch too -- GUI+Tab switching
         * windows only on x86 would be a shortcut nobody could rely on. The
         * keyboard decides them wherever the keys come from (PS/2 there,
         * virtio-input here) and both loops perform them the same way. */
        {
            int sk = keyboard_take_syskey();
            if (sk == SYSKEY_NEXT_WINDOW) compositor_cycle_window();
            else if (sk == SYSKEY_CLOSE_WINDOW) {
                int pid = compositor_close_front();
                if (pid) process_kill((uint32_t)pid);
            }
        }
        compositor_anim_tick();
        arch_cpu_idle();
    }
}
