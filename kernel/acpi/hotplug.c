/* kernel/acpi/hotplug.c -- a processor or a memory stick that was not there
 * a moment ago.
 *
 * `test amldump` has shown \_SB.CPUS with an _EJ0 method on every processor
 * since the AML interpreter was written, and the DSDT has always declared the
 * memory devices too. The firmware has been OFFERING this the whole time and
 * nothing listened. This is the listening.
 *
 * WHAT THE AML ACTUALLY DOES, when you read it, is talk to two small register
 * blocks in I/O space. Every method in that part of the DSDT is a wrapper
 * around "write a slot number here, read a flags byte there". So this talks
 * to the same registers directly, and the result is a few dozen lines instead
 * of an interpreter running a program to do byte-banging on our behalf.
 *
 * THE SELECTOR IS STATE, AND THAT IS THE WHOLE PROTOCOL. Writing it decides
 * which slot every later register in the block refers to. It is not a
 * parameter to a read; it is a mode. Two pieces of code sharing this block
 * without agreeing on that would each read the other's slot.
 *
 * This is POLLED, not interrupt-driven. Hot-plug raises an ACPI general
 * purpose event, and handling GPEs properly means an SCI handler and an event
 * dispatcher this kernel does not have. Polling a handful of I/O ports twice
 * a second costs nothing measurable and gets the same answer a little later,
 * which for something a person did with their hands is soon enough.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/io.h"
#include "acpi/hotplug.h"
#include "mm/pmm.h"
#include "arch/x86_64/smp/smp.h"
#include "drivers/timer/timer.h"

/* The two register blocks. Both addresses are fixed by QEMU's firmware
 * interface and declared in the DSDT's OperationRegions; they are the same on
 * the i440fx and q35 machines. */
/* THE CPU BLOCK IS AT A DIFFERENT ADDRESS ON THE TWO PC CHIPSETS, and there
 * is nothing in either to say which -- the DSDT hard-codes whichever one the
 * machine has. So both are tried, and the one where slot 0 reports a
 * processor present is the right one: slot 0 is the boot processor, which is
 * running this code, so a block that says it is absent is not the block. */
#define CPU_BASE_Q35    0x0CD8
#define CPU_BASE_PIIX4  0xAF00
#define CPU_SELECTOR 0   /* 32-bit, write: which slot the rest refers to */
#define CPU_FLAGS    4   /* 8-bit, read: present/inserting/removing      */
#define CPU_COMMAND  5   /* 8-bit, write                                 */
#define CPU_DATA     8   /* 32-bit, read: the command's answer           */

#define CPU_FLAG_PRESENT   0x01
#define CPU_FLAG_INSERTING 0x02
#define CPU_FLAG_REMOVING  0x04
#define CPU_CMD_GET_ID     3

#define MEM_BASE 0x0A00
#define MEM_ADDR_LO 0x00
#define MEM_ADDR_HI 0x04
#define MEM_SIZE_LO 0x08
#define MEM_SIZE_HI 0x0C
#define MEM_FLAGS   0x14   /* read: enabled/inserting/removing; write: clear */
#define MEM_SELECTOR 0x18  /* 32-bit, write                                  */

#define MEM_FLAG_ENABLED   0x01
#define MEM_FLAG_INSERTING 0x02
#define MEM_FLAG_REMOVING  0x04

#define HOTPLUG_SLOTS 64
#define POLL_INTERVAL_MS 500

static bool g_enabled;
static uint16_t CPU_BASE;
static uint32_t g_cpus_added, g_mem_added;
static uint64_t g_mem_bytes;
static uint64_t g_next_poll_ms;
static bool g_cpu_seen[HOTPLUG_SLOTS];
static bool g_mem_seen[HOTPLUG_SLOTS];

static uint32_t cpu_flags(uint32_t slot) {
    outl(CPU_BASE + CPU_SELECTOR, slot);
    return inb(CPU_BASE + CPU_FLAGS);
}

static uint32_t cpu_apic_id(uint32_t slot) {
    outl(CPU_BASE + CPU_SELECTOR, slot);
    outb(CPU_BASE + CPU_COMMAND, CPU_CMD_GET_ID);
    return inl(CPU_BASE + CPU_DATA);
}

static uint32_t mem_flags(uint32_t slot) {
    outl(MEM_BASE + MEM_SELECTOR, slot);
    return inl(MEM_BASE + MEM_FLAGS);
}

/* Look once at every slot. Called at boot to learn what is already there --
 * so that a later poll can tell "new" from "was always here" -- and then
 * twice a second afterwards. */
static void hotplug_scan(bool first) {
    for (uint32_t slot = 0; slot < HOTPLUG_SLOTS; slot++) {
        uint32_t f = cpu_flags(slot);
        if (!(f & CPU_FLAG_PRESENT)) continue;
        if (first) { g_cpu_seen[slot] = true; continue; }
        if (g_cpu_seen[slot]) continue;

        g_cpu_seen[slot] = true;
        uint32_t apic = cpu_apic_id(slot);
        kprintf("hotplug: a processor was added in slot %u (apic_id %u)\n",
                slot, apic);
        if (smp_hotplug_cpu((uint8_t)apic)) g_cpus_added++;
        /* CLEAR THE INSERT EVENT by writing the same bit back. Leaving it set
         * means the firmware keeps re-reporting the same insertion, and on a
         * machine that actually uses the interrupt it never stops. */
        outl(CPU_BASE + CPU_SELECTOR, slot);
        outb(CPU_BASE + CPU_FLAGS, CPU_FLAG_INSERTING);
    }

    for (uint32_t slot = 0; slot < HOTPLUG_SLOTS; slot++) {
        uint32_t f = mem_flags(slot);
        if (!(f & MEM_FLAG_ENABLED)) continue;
        if (first) { g_mem_seen[slot] = true; continue; }
        if (g_mem_seen[slot]) continue;

        g_mem_seen[slot] = true;
        outl(MEM_BASE + MEM_SELECTOR, slot);
        uint64_t base = (uint64_t)inl(MEM_BASE + MEM_ADDR_LO) |
                        ((uint64_t)inl(MEM_BASE + MEM_ADDR_HI) << 32);
        uint64_t size = (uint64_t)inl(MEM_BASE + MEM_SIZE_LO) |
                        ((uint64_t)inl(MEM_BASE + MEM_SIZE_HI) << 32);
        kprintf("hotplug: %llu MiB of memory appeared at %llx (slot %u)\n",
                (unsigned long long)(size >> 20), (unsigned long long)base, slot);
        if (size && pmm_add_region(base, size)) {
            g_mem_added++;
            g_mem_bytes += size;
        }
        outl(MEM_BASE + MEM_SELECTOR, slot);
        outl(MEM_BASE + MEM_FLAGS, MEM_FLAG_INSERTING);
    }
}

void acpi_hotplug_init(void) {
    /* IS ANYTHING THERE AT ALL? An unimplemented I/O port reads back as
     * all-ones, and a block that IS there reports the boot processor in slot
     * 0 -- which is running this code, so its absence is proof the block is
     * not the one. */
    static const uint16_t bases[2] = { CPU_BASE_Q35, CPU_BASE_PIIX4 };
    for (int i = 0; i < 2; i++) {
        CPU_BASE = bases[i];
        uint32_t probe = cpu_flags(0);
        if (probe != 0xFF && (probe & CPU_FLAG_PRESENT)) { g_enabled = true; break; }
    }
    if (!g_enabled) {
        kprintf("hotplug: no ACPI hot-plug registers on this machine\n");
        return;
    }
    hotplug_scan(true);
    uint32_t present = 0, mem = 0;
    for (uint32_t i = 0; i < HOTPLUG_SLOTS; i++) {
        if (g_cpu_seen[i]) present++;
        if (g_mem_seen[i]) mem++;
    }
    kprintf("hotplug: registers at %04x, watching %u processor slot(s) and "
            "%u memory slot(s)\n", CPU_BASE, present, mem);
}

void acpi_hotplug_poll(void) {
    if (!g_enabled) return;
    uint64_t now = timer_uptime_ms();
    if (now < g_next_poll_ms) return;
    g_next_poll_ms = now + POLL_INTERVAL_MS;
    hotplug_scan(false);
}

bool     acpi_hotplug_available(void) { return g_enabled; }
uint32_t acpi_hotplug_cpus_added(void) { return g_cpus_added; }
uint32_t acpi_hotplug_memory_added(void) { return g_mem_added; }
uint64_t acpi_hotplug_memory_bytes(void) { return g_mem_bytes; }
