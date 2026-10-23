#include "drivers/char/serial.h"
#include "include/io.h"
#include <stdint.h>


#define SERIAL_PORT 0x3F8 // COM1 port

/* --- the receive ring ------------------------------------------------------
 *
 * THE POLLED PATH DROPPED INPUT, and not rarely. serial_init disabled the
 * UART's receive interrupt, so the only reader was the debug REPL polling once
 * per timer tick. At 115200 baud that is ~115 bytes arriving per 10 ms window
 * against a 16-byte FIFO: anything longer than sixteen characters between two
 * polls -- a paste, a scripted command, a `cat` of a file into the console --
 * lost the middle of itself silently.
 *
 * The interrupt drains the FIFO into this ring instead, so the reader's timing
 * stops mattering. 512 bytes is ~45 ms of wire at full rate, which is far more
 * slack than the scheduler needs.
 *
 * Written by the IRQ handler, read by whoever calls serial_read_char().
 * Interrupts are off inside the handler and the readers are short, so the
 * head/tail pair needs no lock -- only `volatile`, so the compiler does not
 * hoist a load of `head` out of a wait. */
#define SERIAL_RING 512
static volatile unsigned char s_ring[SERIAL_RING];
static volatile uint32_t s_head, s_tail;
static volatile uint32_t s_dropped;      /* ring full: counted, never silent */
static volatile int s_irq_driven;        /* 0 until the IRQ is actually wired */

static void serial_rx_push(unsigned char c) {
    uint32_t next = (s_head + 1) % SERIAL_RING;
    if (next == s_tail) { s_dropped++; return; }   /* reader is not keeping up */
    s_ring[s_head] = c;
    s_head = next;
}

/* Drain every byte the FIFO holds. Draining ONE would leave the rest sitting
 * there: this is an edge-ish source in practice, and a handler that returns
 * with data still pending is a handler that can stall until the next byte
 * happens to arrive. */
/* IS THERE A UART AT ALL? A laptop has none, and an I/O port with nothing
 * behind it reads 0xFF -- which to the line-status register means "a byte is
 * waiting" (bit 0) AND "ready to transmit" (bit 5), forever. The drain below
 * was an infinite loop on every such machine, and the transmit wait only
 * escaped by luck of the same bit. Starts TRUE so the bytes written before
 * serial_init() ran still go out on a machine that has one. */
static int g_uart_present = 1;

int serial_present(void) { return g_uart_present; }

void serial_irq_drain(void) {
    if (!g_uart_present) return;
    /* Bounded even with a UART: a FIFO that reports data forever is a broken
     * part, and a broken part must not be able to stop the machine. */
    for (int n = 0; n < 4096 && (inb(SERIAL_PORT + 5) & 0x01); n++)
        serial_rx_push((unsigned char)inb(SERIAL_PORT));
}

uint32_t serial_rx_dropped(void) { return s_dropped; }

void serial_init(void) {
    /* THE SCRATCH REGISTER: a byte written to it reads back on every 16450 and
     * later, and on an empty port it reads 0xFF whatever was written. Two
     * patterns, because 0xFF is itself one of the values a real part could
     * hold. */
    outb(SERIAL_PORT + 7, 0x5A);
    uint8_t a = inb(SERIAL_PORT + 7);
    outb(SERIAL_PORT + 7, 0xA5);
    uint8_t b = inb(SERIAL_PORT + 7);
    if (a != 0x5A || b != 0xA5 || inb(SERIAL_PORT + 5) == 0xFF) {
        g_uart_present = 0;
        return;
    }
    outb(SERIAL_PORT + 1, 0x00); // Disable all interrupts
    outb(SERIAL_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(SERIAL_PORT + 1, 0x00); //                  (hi byte)
    outb(SERIAL_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
}



//helper function

static int serial_is_ready() {
    return inb(SERIAL_PORT + 5) & 0x20;
}


void serial_write_char(char c) {
    if (!g_uart_present) return;
    // Wait for the transmit buffer to be empty -- for a while, not forever: a
    // UART that never drains is a stuck part, and dropping a byte of log is
    // the right price for a machine that keeps booting.
    for (int spin = 0; spin < 200000 && !serial_is_ready(); spin++) { }
    outb(SERIAL_PORT, c);
}

/* --- input ----------------------------------------------------------------
 *
 * Interrupt-driven once serial_irq_enable() has run, polled before that. Both
 * answers come from the same two functions so every existing caller -- the
 * debug REPL, the early-boot paths that run before the IDT exists -- keeps
 * working unchanged.
 *
 * The fallback is not a nicety: serial output and input are wanted during
 * bring-up, long before there is an IOAPIC to route an IRQ through. */

// 1 iff a byte is waiting, else 0.
int serial_has_char(void) {
    if (s_irq_driven)
        return s_head != s_tail;
    if (!g_uart_present) return 0;
    return inb(SERIAL_PORT + 5) & 0x01;
}

// Read one received byte. Caller must have checked serial_has_char() first
// (same contract as keyboard_has_char()/keyboard_getchar()).
char serial_read_char(void) {
    if (s_irq_driven) {
        if (s_head == s_tail) return 0;
        unsigned char c = s_ring[s_tail];
        s_tail = (s_tail + 1) % SERIAL_RING;
        return (char)c;
    }
    return (char)inb(SERIAL_PORT);
}

/* Turn on the UART's Received Data Available interrupt and start believing the
 * ring. Called once the IRQ is routed -- never before, or bytes would land in
 * a ring nobody is filling. */
void serial_irq_enable(void) {
    if (!g_uart_present) return;        /* no part, no interrupt to believe in */
    serial_irq_drain();                 /* anything already in the FIFO */
    outb(SERIAL_PORT + 1, 0x01);        /* IER bit 0: Received Data Available */
    s_irq_driven = 1;
}


void serial_write_string(const char *str) {
    while (*str) {
        serial_write_char(*str++);
    }
}


void serial_write_hex(uint64_t value) {
    const char *hex_digits = "0123456789ABCDEF"; 
    char buffer[17]; // 16 hex digits + null terminator
    buffer[16] = '\0';
    
    for (int i = 15; i >= 0; i--) {
        buffer[i] = hex_digits[value & 0xF];
        value >>= 4;
    }
    serial_write_string("0x");
    serial_write_string(buffer);
}




