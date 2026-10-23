#ifndef _AARCH64_PL011_H
#define _AARCH64_PL011_H

#include <stdint.h>

/* The ARM PrimeCell PL011 UART -- the console for the whole aarch64 campaign.
 *
 * This is the aarch64 counterpart of kernel/drivers/char/serial.h (a 16550 at
 * port 0x3F8), and the function names deliberately mirror it: when the console
 * HAL is DERIVED in A3-A5 (docs/ARM64.md §2.3) the two should visibly be the
 * same shape, so that the seam falls out rather than being invented.
 *
 * It lives under arch/aarch64/ rather than kernel/drivers/char/ because right
 * now its base address is a hardcoded QEMU-`virt` constant. It earns a move to
 * drivers/ when A2 reads that address out of the device tree instead. */

void pl011_init(void);

/* Re-point the driver at the MMIO window once the MMU is on, so it stops
 * depending on the boot identity map (which A5 removes). Pass
 * MMIO_BASE + 0x09000000. Safe to call at any time; the UART is stateless
 * between accesses. */
void pl011_use_mmio_window(unsigned long base);
void pl011_putc(char c);
void pl011_puts(const char *s);

/* Fixed-width hex, `0x` prefixed. Register dumps line up, which is the entire
 * point once A1 starts printing ESR/ELR/FAR next to each other. */
void pl011_puthex64(uint64_t v);
void pl011_puthex32(uint32_t v);

/* Unsigned decimal. No padding. */
void pl011_putdec(uint64_t v);

/* Polled input, same contract as serial_has_char()/serial_read_char(). */
int  pl011_has_char(void);

/* Interrupt-driven receive -- see the ring comment in pl011.c. */
void     pl011_irq_drain(void);
void     pl011_irq_enable(void);
uint32_t pl011_rx_dropped(void);
char pl011_getc(void);

#endif /* _AARCH64_PL011_H */
