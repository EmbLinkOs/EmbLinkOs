#ifndef __XHCI_H__
#define __XHCI_H__

#include "drivers/usb/usb.h"

bool xhci_init_controller(struct usb_controller *ctrl);

// Route each controller's PCI interrupt and register the handler. Call once,
// after enumeration, to switch HID input from polling to interrupt-driven.
void xhci_enable_irq(void);

// Interrupt handler: acknowledge + drain every controller's event ring. Wired to
// the IRQ vector by xhci_enable_irq (exposed for the IRQ dispatch layer).
void xhci_irq(void);
// Drain the event ring of every controller not serviced by interrupt -- all
// of them on aarch64. Called from usb_poll().
void xhci_poll(void);
// HID reports processed so far, keyboard and mouse. Evidence that input is
// ARRIVING, not merely that a device was configured.
void xhci_hid_stats(uint64_t *kbd, uint64_t *mouse);

/* Interrupts counted across controllers, and whether any has been given up on
 * and is serviced by polling (see xhci_poll). `test xhci stats`. */
void xhci_irq_stats(uint64_t *irqs, bool *polled);
/* Mask every controller's interrupt and forget it ever fired: the state a
 * laptop with broken routing is in. `test xhci nointr`; the fallback must
 * then engage within its grace period. */
void xhci_force_nointr(void);

/* Devices addressed on xHCI controllers. Counted separately from
 * usb_device_count() because xHCI does not use usb_core's device table -- see
 * the .c. */
uint32_t xhci_device_count(void);

/* Print every xHCI port's PORTSC and retry any connected port that has no
 * slot, loudly. For the no-root diagnostic -- see xhci.c. */
void xhci_report_ports(void);

#endif /* __XHCI_H__ */
