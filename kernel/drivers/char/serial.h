#ifndef _SERIAL_H
#define _SERIAL_H

#include <stdint.h>



void serial_init(void);
void serial_write_char(char c);
void serial_write_string(const char *str);
void serial_write_hex(uint64_t value);

/* Input (COM1). serial_has_char() is a non-blocking data-ready check;
 * serial_read_char() reads one byte and must only be called after it.
 *
 * INTERRUPT-DRIVEN once serial_irq_enable() has run, POLLED before that --
 * both through these same two functions, so early-boot callers that run before
 * there is an IOAPIC to route through keep working unchanged.
 *
 * The polled path DROPPED INPUT, and not rarely: at 115200 baud roughly 115
 * bytes arrive per 10 ms timer tick against a 16-byte FIFO, so anything longer
 * than sixteen characters pasted or scripted between two polls lost its
 * middle, silently. serial_irq_drain() is the handler's body. */
int  serial_has_char(void);
char serial_read_char(void);
void     serial_irq_drain(void);
void     serial_irq_enable(void);
uint32_t serial_rx_dropped(void);

#endif // _SERIAL_H
