#include "include/arch_ipi.h"
#include "arch/x86_64/irq/lapic.h"
#include "arch/x86_64/irq/idt.h"
#include "include/percpu.h"
#include "include/kprintf.h"
#include "mm/vmm.h"

/* The x86 half of include/arch_ipi.h.
 *
 * An IPI here is a write to the local APIC's Interrupt Command Register, and
 * the VECTOR IS THE MESSAGE -- there is no payload, so the only way to say
 * which of three things is meant is to send a different vector. 0xF0..0xF2 sit
 * above every device vector and below the spurious vector (0xFF).
 *
 * "All excluding self" is a DESTINATION SHORTHAND the hardware understands
 * (ICR bits 19:18 = 0b11), so a broadcast is one register write regardless of
 * how many cores there are -- no loop, no list of APIC ids, and no chance of
 * missing a core that came up after the list was built.
 */

#define IPI_VECTOR_BASE   0xF0

extern void lapic_ipi_stub_0(void);
extern void lapic_ipi_stub_1(void);
extern void lapic_ipi_stub_2(void);

void lapic_ipi_handler(uint64_t reason);
void lapic_ipi_handler(uint64_t reason) {
    /* EOI FIRST. Unlike a device interrupt there is nothing to quiesce -- the
     * sender is not asserting a line -- and ipi_dispatch() may not return at
     * all (IPI_HALT parks the core forever). An un-EOI'd IPI would leave this
     * core's APIC at that priority, blocking every later interrupt, on a core
     * that is otherwise fine. */
    lapic_send_eoi();
    ipi_dispatch((enum ipi_reason)reason);
}

void arch_ipi_init_this_cpu(void) {
    /* 0x8E: present, DPL 0, 64-bit interrupt gate -- interrupts masked on
     * entry, exactly as the timer's is. The IDT is shared between cores, so
     * after the first core does this the rest are re-writing the same three
     * entries with the same values, which is harmless and simpler than
     * tracking whether it has been done. */
    idt_set_entry(IPI_VECTOR_BASE + 0, (uint64_t)lapic_ipi_stub_0, 0x8E);
    idt_set_entry(IPI_VECTOR_BASE + 1, (uint64_t)lapic_ipi_stub_1, 0x8E);
    idt_set_entry(IPI_VECTOR_BASE + 2, (uint64_t)lapic_ipi_stub_2, 0x8E);
}

bool arch_ipi_broadcast(enum ipi_reason reason) {
    if (reason >= IPI_REASON_COUNT || cpu_count <= 1)
        return false;

    lapic_send_ipi_all_but_self((uint8_t)(IPI_VECTOR_BASE + reason));
    return true;
}

/* This core's TLB, and only this core's -- the broadcast is the caller's job.
 * Reloading CR3 flushes every non-global entry, which is what a shootdown
 * needs and is cheaper than walking a list of addresses we were not told. */
void arch_tlb_flush_all_local(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}
