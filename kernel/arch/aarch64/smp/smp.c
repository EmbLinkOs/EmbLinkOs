#include "arch/aarch64/boot/fdt.h"
#include "arch/aarch64/cpu/percpu.h"
#include "arch/aarch64/cpu/cpu_features.h"
#include "arch/aarch64/irq/gicv3.h"
#include "arch/aarch64/irq/exception.h"
#include "arch/aarch64/mm/pagetable.h"
#include "drivers/timer/timer.h"
#include "include/arch_irq.h"
#include "include/arch_ipi.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "mm/pmm.h"
#include "process/process.h"
#include "power/power.h"   /* idle residency accounting */

/* Secondary CPU bring-up over PSCI -- docs/ARM64.md phase A9.
 *
 * PSCI (the Power State Coordination Interface) is the ARM world's answer to
 * x86's INIT-SIPI-SIPI dance, and it is a great deal less ceremony: ONE call,
 * with a target core, a physical entry address and a value to hand it. There
 * is no trampoline to place in low memory, no real-mode stub, no waiting on an
 * APIC. What there IS, and x86 does not have, is a CONDUIT question -- the
 * call reaches firmware through either `hvc` or `smc`, and which one is a
 * property of the machine that only the device tree knows.
 *
 * The hard part is not the call. It is that everything a core needs is
 * PER-CORE state the primary cannot set on its behalf: its stack, its
 * TPIDR_EL1, its GIC redistributor and CPU interface, its timer. Each is done
 * by the core itself, in smp_secondary_main(), in the order core 0 did them.
 */

/* PSCI function IDs (SMC Calling Convention, 64-bit). */
#define PSCI_CPU_ON_64      0xC4000003u
#define PSCI_SUCCESS                 0
#define PSCI_ALREADY_ON             (-4)

enum psci_conduit { PSCI_NONE = 0, PSCI_HVC, PSCI_SMC };
static enum psci_conduit g_conduit;
static uint32_t          g_cpu_on_id = PSCI_CPU_ON_64;

/* Every core's MPIDR affinity value, indexed BY CPU INDEX. The device tree
 * lists them; nothing else does. */
static uint64_t g_cpu_mpidr[MAX_CPUS];

/* A core's MPIDR affinity, by dense cpu index -- what a TARGETED SGI needs to
 * build its affinity fields. Exposed rather than duplicated: the device-tree
 * walk that filled this array is the only thing that knows the mapping, and a
 * second reading of /cpus would be a second chance to disagree with it. */
uint64_t smp_cpu_mpidr(uint32_t cpu) {
    return cpu < MAX_CPUS ? g_cpu_mpidr[cpu] : 0;
}
static uint32_t g_cpu_listed;

/* Each secondary's stack top, as a KERNEL-WINDOW VIRTUAL address. boot.S reads
 * this table with the MMU still off and converts down by KERNEL_VIRT_BASE --
 * see the comment there. Not static: boot.S names it. */
#define SMP_STACK_SIZE (16 * 1024)           /* as a kernel stack is */
uint64_t smp_stack_top[MAX_CPUS];

/* The stacks themselves, in .bss AND NOT ON THE HEAP.
 *
 * That is a correctness requirement, not a preference: boot.S converts these
 * addresses to physical with a plain subtraction of KERNEL_VIRT_BASE, and that
 * identity holds ONLY for the kernel image window. kmalloc() hands back
 * kernel-HEAP memory, which is mapped somewhere else entirely -- so the
 * arithmetic produced a plausible-looking physical address that pointed at
 * nothing, and every secondary faulted on its first push with no console to
 * say so. The symptom was three cores that PSCI accepted and that never
 * reported in.
 *
 * 16 KiB per core of permanently-reserved .bss is the price, and it is the
 * same trade boot.S already makes for core 0's stack. */
static uint8_t g_smp_stacks[MAX_CPUS][SMP_STACK_SIZE] __attribute__((aligned(16)));

/* Cores that have finished bringing THEMSELVES up. Written by the secondary,
 * spun on by the primary. */
static volatile uint32_t g_online;

static bool psci_probe(void);

static int64_t psci_call(uint32_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;

    if (g_conduit == PSCI_HVC)
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

/* PSCI is not only how secondary cores start -- it is also how this machine
 * turns off and reboots (kernel/arch/aarch64/power/power_arm.c). The probe is
 * therefore made available on its own, and made idempotent, rather than
 * copied: a second implementation of "which conduit does this firmware use"
 * is a second place to get it wrong. */
static bool g_probed = false;
static bool g_probe_ok = false;

bool psci_available(void) {
    if (!g_probed) {
        g_probed = true;
        g_probe_ok = fdt_ok() && psci_probe();
    }
    return g_probe_ok;
}

int64_t psci_invoke(uint32_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!psci_available())
        return -1;              /* PSCI NOT_SUPPORTED */
    return psci_call(fn, a1, a2, a3);
}

static bool psci_probe(void) {
    fdt_node_t n = fdt_find_compatible("arm,psci-1.0");
    if (n == FDT_NONE) n = fdt_find_compatible("arm,psci-0.2");
    if (n == FDT_NONE) n = fdt_find_compatible("arm,psci");
    if (n == FDT_NONE) {
        kprintf("smp: no PSCI node in the device tree -- cannot start a second core\n");
        return false;
    }

    if (fdt_prop_has_string(n, "method", "hvc"))
        g_conduit = PSCI_HVC;
    else if (fdt_prop_has_string(n, "method", "smc"))
        g_conduit = PSCI_SMC;
    else {
        kprintf("smp: the PSCI node names neither hvc nor smc\n");
        return false;
    }

    /* A 0.1-era tree carries its own function IDs; 0.2 and later standardise
     * them and QEMU does not override. Honouring the property when present
     * costs one line and is the difference between working on this machine and
     * working on the ones that do. */
    g_cpu_on_id = fdt_prop_u32(n, "cpu_on", PSCI_CPU_ON_64);

    kprintf("smp: PSCI via %s, CPU_ON = %p [from the device tree]\n",
            g_conduit == PSCI_HVC ? "hvc" : "smc",
            (void *)(uintptr_t)g_cpu_on_id);
    return true;
}

/* Walk /cpus. A core's `reg` is the MPIDR affinity value PSCI wants as its
 * target, and its ORDER in the tree defines the CPU index -- there is no other
 * numbering. The boot core is identified by MATCHING MPIDR rather than assumed
 * to be first, then moved to index 0, because cpu_table[0], this_cpu() before
 * TPIDR_EL1 is set, and the "core 0 does it once" split in gic_init all assume
 * that and it is cheaper to make true here than to teach each of them. */
static void cpus_probe(void) {
    uint64_t self;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
    self &= 0xFF00FFFFFFULL;

    /* /cpus, then ITS children -- not fdt_find_device_type(), which searches
     * only the root's DIRECT children and therefore finds no CPU at all: they
     * are grandchildren. (It found zero, reported "nothing to start", and was
     * entirely truthful about a question it had asked wrong.) */
    fdt_node_t cpus = FDT_NONE;
    for (fdt_node_t n = fdt_first_child(fdt_root()); n != FDT_NONE;
         n = fdt_next_sibling(n)) {
        const char *nm = fdt_node_name(n);
        if (nm && strcmp(nm, "cpus") == 0) { cpus = n; break; }
    }
    if (cpus == FDT_NONE) {
        kprintf("smp: no /cpus node in the device tree\n");
        return;
    }

    /* A cpu node's `reg` is decoded with #address-cells FROM ITS PARENT, and
     * /cpus declares its own -- 1 on `virt`, 2 on machines with more than 255
     * cores or a multi-cluster MPIDR. Reading it rather than assuming is the
     * difference between finding core 1 and finding whatever the second half
     * of core 0's affinity looks like. */
    uint32_t ac = fdt_prop_u32(cpus, "#address-cells", 1);
    if (ac != 1 && ac != 2) ac = 1;

    for (fdt_node_t n = fdt_first_child(cpus); n != FDT_NONE && g_cpu_listed < MAX_CPUS;
         n = fdt_next_sibling(n)) {
        uint32_t len = 0;
        const char *dt = (const char *)fdt_prop(n, "device_type", &len);
        if (!dt || !len || strcmp(dt, "cpu") != 0)
            continue;

        const uint8_t *reg = (const uint8_t *)fdt_prop(n, "reg", &len);
        if (!reg || len < ac * 4)
            continue;

        /* Big-endian cells, as everything in a flattened device tree is. */
        uint64_t v = 0;
        for (uint32_t c = 0; c < ac; c++) {
            uint32_t w = ((uint32_t)reg[c * 4] << 24) | ((uint32_t)reg[c * 4 + 1] << 16) |
                         ((uint32_t)reg[c * 4 + 2] << 8) | (uint32_t)reg[c * 4 + 3];
            v = (v << 32) | w;
        }
        g_cpu_mpidr[g_cpu_listed++] = v & 0xFF00FFFFFFULL;
    }

    for (uint32_t i = 1; i < g_cpu_listed; i++) {
        if (g_cpu_mpidr[i] == self) {
            g_cpu_mpidr[i] = g_cpu_mpidr[0];
            g_cpu_mpidr[0] = self;
            break;
        }
    }
}

void smp_secondary_main(uint64_t index);
void smp_secondary_main(uint64_t index) {
    /* EXCEPTION VECTORS FIRST, before anything that could fault and before any
     * interrupt can be unmasked. VBAR_EL1 is a BANKED, PER-CORE register: core
     * 0 installing the table at A1 says nothing about core 1, whose VBAR_EL1 is
     * whatever reset left in it.
     *
     * Without this a secondary comes up looking perfectly healthy -- it
     * reports online, its redistributor is awake, its timer is armed, DAIF says
     * interrupts are unmasked -- and takes ZERO interrupts, because every one
     * of them vectors to an address with nothing at it. The core never
     * preempts, never runs a scheduled thread, and never says so. That is
     * exactly how this presented: "4 of 4 cores online" and three cores that
     * had taken 0 interrupts between them. */
    exception_init();

    /* PAN before anything can touch user memory on this core. Per core because
     * SCTLR_EL1 and PSTATE are per core -- a secondary that skipped it would
     * be the one core where the kernel can still read any user page, with
     * nothing to show for it. Same reasoning as the x86 side's CR4. */
    arm_protection_init_this_cpu();

    /* TPIDR_EL1 next: this_cpu() is a read of it, and everything below --
     * including anything that takes a lock or reports a failure -- wants a
     * correct answer from it. */
    percpu_init_this_cpu((uint32_t)index);

    /* This core's half of the interrupt controller. The distributor is already
     * configured (core 0 did it once); what is per-core is waking the
     * redistributor and enabling the ICC_* system register interface. */
    if (gic_init_this_cpu() != 0) {
        kprintf("smp: cpu%d: GIC per-CPU init failed; parking\n", (int)index);
        for (;;) arch_cpu_idle();
    }

    /* Its own timer. The generic timer is per-core state -- CNTV_CTL_EL0 and
     * CNTV_TVAL_EL0 are banked -- so a core that skips this never gets a tick
     * and therefore never preempts anything. */
    timer_init_this_cpu();

    /* This core's SGIs. Per core because SGIs live in each core's own
     * redistributor -- enabling them on core 0 says nothing about core 1, and
     * a core that skips this takes an unhandled interrupt on the first IPI. */
    arch_ipi_init_this_cpu();

    this_cpu()->online = true;
    __atomic_add_fetch(&g_online, 1, __ATOMIC_RELEASE);
    kprintf("smp: cpu%d online\n", (int)index);

    /* Join the scheduler. process.c is already SMP-aware -- g_sched_lock, the
     * per-CPU cur_thread, a per-core idle thread -- so there is nothing
     * aarch64-specific left: adopt a context, enable interrupts, and let this
     * core's timer dispatch work exactly as core 0's does. */
    if (!process_adopt_current()) {
        kprintf("smp: cpu%d: could not adopt a boot context; parking\n", (int)index);
        for (;;) arch_cpu_idle();
    }

    arch_irq_enable();
    {
        uint64_t daif;
        __asm__ volatile("mrs %0, daif" : "=r"(daif));
        kprintf("smp: cpu%d scheduling, DAIF=%p\n", (int)index,
                (void *)(uintptr_t)daif);
    }
    for (;;) {
        /* Bracketed for the idle accounting (kernel/power/power.h). This --
         * NOT the per-core idle kthread -- is where a secondary actually
         * spends its time: the adopted boot thread is NORMAL priority and
         * always runnable, so the PRIORITY_BACKGROUND idle kthread behind it
         * is a liveness backstop that is never reached in practice. */
        sched_idle_enter();
        timer_arm_this_cpu_ms(sched_idle_next_ms());

        power_idle_enter();
        arch_cpu_idle();
        power_idle_exit();

        sched_idle_exit();
    }
}

extern void secondary_entry(void);      /* boot.S */

void smp_bringup(void) {
    kprintf("\n=== SMP bring-up (PSCI) ===\n");

    if (!fdt_ok() || !psci_probe())
        return;

    cpus_probe();
    kprintf("smp: %d cpu(s) in the device tree\n", (int)g_cpu_listed);
    if (g_cpu_listed <= 1) {
        /* Still report the tally. A uniprocessor machine is a legitimate
         * configuration, not a failure, and a summary line that appears only
         * when there is something to boast about is a summary line no test can
         * assert on. */
        cpu_count = 1;
        kprintf("smp: 1 of 1 core(s) online\n");
        return;
    }

    /* PSCI wants a PHYSICAL entry address and secondary_entry's linked address
     * is virtual -- the kernel runs in the higher half. */
    uint64_t entry = KV2P((uint64_t)(uintptr_t)&secondary_entry);

    /* Reopen the identity map for the length of the bring-up. Each secondary
     * runs boot.S's enable_mmu, which installs boot_l0_ttbr0 and turns the MMU
     * on while the PC is still PHYSICAL -- the next fetch translates through
     * this map or not at all. A2 dropped it on purpose and it is dropped again
     * below; leaving it in place would give every user process 4 GiB of
     * physical memory mapped at address 0 in its own TTBR0 half. */
    vm_restore_identity_map();

    for (uint32_t i = 1; i < g_cpu_listed; i++) {
        /* A stack per core, published BEFORE it is started: the first thing
         * secondary_entry does with the MMU off is read this table, so a core
         * started before its stack exists runs on whatever zero translates to. */
        smp_stack_top[i] = (uint64_t)(uintptr_t)&g_smp_stacks[i][SMP_STACK_SIZE];

        /* Read by a core whose caches are not on yet, so the write has to be
         * in memory and not just in ours. */
        __asm__ volatile("dsb ish" ::: "memory");

        uint32_t before = g_online;
        int64_t rc = psci_call(g_cpu_on_id, g_cpu_mpidr[i], entry, i);
        if (rc != PSCI_SUCCESS && rc != PSCI_ALREADY_ON) {
            kprintf("smp: cpu%d: PSCI CPU_ON refused (%d)\n", (int)i, (int)rc);
            continue;
        }

        /* Wait for the core to say so ITSELF. "Started N cores" that counts
         * PSCI's return value rather than the cores' own reports is exactly
         * the claim that cannot fail. Bounded, so a core that never arrives
         * produces a diagnosis instead of a hang. */
        int spins = 200000000;
        while (__atomic_load_n(&g_online, __ATOMIC_ACQUIRE) == before && --spins)
            arch_cpu_relax();
        if (!spins)
            kprintf("smp: cpu%d: started but never reported in\n", (int)i);
    }

    /* Close it again. Every secondary that got this far is executing in the
     * higher half and no longer needs it. */
    vm_drop_identity_map();

    cpu_count = 1 + g_online;
    kprintf("smp: %d of %d core(s) online\n", (int)cpu_count, (int)g_cpu_listed);
}
