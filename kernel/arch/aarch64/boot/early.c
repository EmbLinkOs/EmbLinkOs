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
#include "fs/vfs.h"
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

    /* --- the real filesystem -------------------------------------------------
     * process_init() first: EMBKFS takes sleeping locks, and a sleeping lock
     * needs a scheduler to sleep on. Then vfs_init(), then embkfs_init(),
     * which probes every registered block device and mounts what it finds --
     * exactly the sequence kernel/main.c runs on x86, against exactly the same
     * code. */
    kprintf("\n--- filesystem ---\n");
    process_init();
    vfs_init();
    embkfs_init();

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

    kprintf("\n--- all self-tests done: %d failure(s) ---\n", (int)selftest_fails);

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
        compositor_anim_tick();
        arch_cpu_idle();
    }
}
