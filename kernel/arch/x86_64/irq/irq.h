#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdint.h>


// Fonction signatures for IRQ handling
typedef void (*irq_handler_t)(void);

// Resiger a handler for IRQ N (0 - 15). Also unmasks the IRQ at the PIC.
void irq_register(uint8_t irq, irq_handler_t handler);

// Unregister handler and mask the IRQ
void irq_unregister(uint8_t irq);

// Called from main.c -installs all 16 IRQ stubs into IDT
void irq_install(void);

/* ---- EmbDBG v2: Interrupt Viewer -----------------------------------------
 * Per-IRQ-line delivery counters + a snapshot of the handler table, for the
 * `interrupts` / `irq` shell commands. Counters are incremented in irq_handler
 * (the hardware-IRQ dispatch); the LAPIC timer (vector 48) is off this path --
 * count it via lapic_timer_get_ticks() instead. */
struct irq_line_info {
    uint64_t count;         /* deliveries seen on this line (0..15) */
    uint64_t handler_addr;  /* registered handler fn ptr, 0 if none */
};
/* Copy all 16 IRQ lines' {count, handler} into out[16]. */
void irq_snapshot(struct irq_line_info out[16]);

#endif /* __IRQ_H__ */