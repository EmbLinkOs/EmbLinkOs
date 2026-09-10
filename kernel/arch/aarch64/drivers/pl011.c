#include "arch/aarch64/drivers/pl011.h"
#include "arch/aarch64/boot/fdt.h"
#include "arch/aarch64/irq/gicv3.h"
#include "drivers/char/serial.h"

/* PL011 on QEMU `-M virt`. Hardcoded, and honestly so: `virt`'s memory map is
 * a documented, stable contract (hw/arm/virt.c, VIRT_UART), and docs/ARM64.md
 * §5 rules out a general device-tree driver framework for exactly this reason
 * -- we read the few nodes we need and hardcode the rest of one board.
 *
 * Register offsets: ARM PrimeCell UART (PL011) Technical Reference Manual. */
#define PL011_PHYS      0x09000000UL

/* Where the UART is REACHED from, which is not the same as where it is.
 *
 * Before the MMU there is only the physical address. After it, the physical
 * address happens to still work -- TTBR0 holds an identity map -- but only
 * until A5 gives TTBR0 to user space, at which point every device access in
 * the kernel would fault at once. So the base is switched to the MMIO window
 * (kernel/mm/pmm.h's MMIO_BASE) as soon as there is one, and the identity map
 * is never depended on for anything that has to outlive it.
 *
 * volatile, and written exactly once from pl011_use_mmio_window(). */
static volatile unsigned long pl011_base = PL011_PHYS;

#define UARTDR          0x00    /* data */
#define UARTFR          0x18    /* flag */
#define UARTIBRD        0x24    /* integer baud rate divisor */
#define UARTFBRD        0x28    /* fractional baud rate divisor */
#define UARTLCR_H       0x2C    /* line control */
#define UARTCR          0x30    /* control */
#define UARTIMSC        0x38    /* interrupt mask set/clear */
#define UARTICR         0x44    /* interrupt clear */

/* PL011 interrupt bits (ARM DDI 0183, table 3-14). RX is "the FIFO reached its
 * trigger level"; RT is "there is something in the FIFO and nothing has
 * arrived for a while". BOTH are needed: with RX alone, a short burst that
 * never reaches the trigger level sits in the FIFO until the next byte turns
 * up -- which for a human typing is the difference between a responsive
 * console and one that echoes a character late, forever. */
#define IMSC_RX         (1u << 4)
#define IMSC_RT         (1u << 6)
#define ICR_RX          (1u << 4)
#define ICR_RT          (1u << 6)

#define FR_RXFE         (1u << 4)   /* receive FIFO empty */
#define FR_TXFF         (1u << 5)   /* transmit FIFO full */
#define FR_BUSY         (1u << 3)

#define LCR_H_FEN       (1u << 4)   /* enable FIFOs */
#define LCR_H_WLEN_8    (3u << 5)   /* 8 data bits */

#define CR_UARTEN       (1u << 0)
#define CR_TXE          (1u << 8)
#define CR_RXE          (1u << 9)

/* volatile, and via a function, so the compiler cannot reorder or elide device
 * accesses the way it may for ordinary memory. */
static inline void mmio_w32(unsigned long off, uint32_t v) {
    *(volatile uint32_t *)(pl011_base + off) = v;
}
static inline uint32_t mmio_r32(unsigned long off) {
    return *(volatile uint32_t *)(pl011_base + off);
}

void pl011_use_mmio_window(unsigned long base) {
    pl011_base = base;
}

void pl011_init(void) {
    /* QEMU hands us a UART that already works, so none of this is strictly
     * required under -M virt. It is here because the first time this code
     * meets firmware that left the UART in some other state -- a different
     * baud, FIFOs off, RX disabled -- the symptom is a blank console with no
     * way to ask why. Programming it costs nine stores. */

    mmio_w32(UARTCR, 0);                        /* disable while reconfiguring */

    /* 115200 baud from `virt`'s 24 MHz UART clock:
     *   divisor = 24e6 / (16 * 115200) = 13.0208
     *   IBRD    = 13
     *   FBRD    = round(0.0208 * 64) = 1                                  */
    mmio_w32(UARTIBRD, 13);
    mmio_w32(UARTFBRD, 1);

    mmio_w32(UARTLCR_H, LCR_H_WLEN_8 | LCR_H_FEN);   /* 8N1, FIFOs on */

    mmio_w32(UARTIMSC, 0);                      /* polled: mask every source */
    mmio_w32(UARTICR, 0x7FF);                   /* clear anything pending */

    mmio_w32(UARTCR, CR_UARTEN | CR_TXE | CR_RXE);
}

void pl011_putc(char c) {
    /* A bare '\n' leaves the cursor in column N on a real terminal, so the
     * second line of a panic dump starts halfway across the screen. The 16550
     * driver does the same translation. */
    if (c == '\n')
        pl011_putc('\r');

    while (mmio_r32(UARTFR) & FR_TXFF)
        ;
    mmio_w32(UARTDR, (uint32_t)(unsigned char)c);
}

void pl011_puts(const char *s) {
    while (*s)
        pl011_putc(*s++);
}

static void puthex(uint64_t v, int nibbles) {
    static const char digits[] = "0123456789abcdef";
    pl011_puts("0x");
    for (int i = nibbles - 1; i >= 0; i--)
        pl011_putc(digits[(v >> (i * 4)) & 0xf]);
}

void pl011_puthex64(uint64_t v) { puthex(v, 16); }
void pl011_puthex32(uint32_t v) { puthex(v, 8); }

void pl011_putdec(uint64_t v) {
    char buf[21];               /* 2^64-1 is 20 digits, plus the terminator */
    int i = 0;

    if (v == 0) {
        pl011_putc('0');
        return;
    }
    while (v && i < (int)sizeof(buf) - 1) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--)
        pl011_putc(buf[i]);
}

/* --- the receive ring ------------------------------------------------------
 * Same story as the 16550's, and for the same reason: the debug console polled
 * once per timer tick, and a PL011's FIFO is 32 bytes. Anything longer than
 * that pasted or scripted between two polls lost its middle, silently. The
 * interrupt drains the FIFO into this ring instead, so the reader's timing
 * stops mattering. */
#define PL011_RING 512
static volatile unsigned char s_ring[PL011_RING];
static volatile uint32_t s_head, s_tail, s_dropped;
static volatile int s_irq_driven;

void pl011_irq_drain(void) {
    while ((mmio_r32(UARTFR) & FR_RXFE) == 0) {
        unsigned char c = (unsigned char)(mmio_r32(UARTDR) & 0xff);
        uint32_t next = (s_head + 1) % PL011_RING;
        if (next == s_tail) { s_dropped++; continue; }
        s_ring[s_head] = c;
        s_head = next;
    }
    mmio_w32(UARTICR, ICR_RX | ICR_RT);      /* ack receive + receive-timeout */
}

uint32_t pl011_rx_dropped(void) { return s_dropped; }

static void pl011_irq_handler(uint32_t intid) {
    (void)intid;
    pl011_irq_drain();
}

/* Find the UART's interrupt in the device tree and wire it up. The same shape
 * timer_generic.c uses for the generic timer: the tree is the only thing that
 * knows which INTID this board's UART is on, and hardcoding `virt`'s number
 * would work on exactly one machine. */
void pl011_irq_enable(void) {
    fdt_node_t node = fdt_find_compatible("arm,pl011");
    uint32_t type, num, flags;
    if (node == FDT_NONE || !fdt_interrupt(node, 0, &type, &num, &flags)) {
        /* No interrupt in the tree: stay POLLED. Silently switching the
         * readers to a ring nothing fills would be a console that never
         * receives another byte. */
        pl011_puts("pl011: no interrupt in the device tree -- input stays polled\n");
        return;
    }

    uint32_t intid = gic_intid(type, num);
    if (gic_register(intid, pl011_irq_handler, "pl011 rx") != 0) {
        pl011_puts("pl011: could not register the receive interrupt -- staying polled\n");
        return;
    }

    pl011_irq_drain();                       /* whatever is already queued */
    mmio_w32(UARTIMSC, IMSC_RX | IMSC_RT);   /* RX and RX-timeout */
    s_irq_driven = 1;
}

int pl011_has_char(void) {
    if (s_irq_driven)
        return s_head != s_tail;
    return (mmio_r32(UARTFR) & FR_RXFE) == 0;
}

char pl011_getc(void) {
    if (s_irq_driven) {
        while (s_head == s_tail)
            ;                                /* the polled contract, kept */
        unsigned char c = s_ring[s_tail];
        s_tail = (s_tail + 1) % PL011_RING;
        return (char)c;
    }
    while (!pl011_has_char())
        ;
    return (char)(mmio_r32(UARTDR) & 0xff);
}

/* --- kernel/drivers/char/serial.h ------------------------------------------
 *
 * The 16550 on x86 and the PL011 here implement the SAME header, which is what
 * makes kernel/lib/kprintf.c and kernel/mm/pmm.c compile unchanged for both.
 * That header was written for a specific UART and turned out to describe a
 * console; adopting it rather than inventing a new interface is docs/ARM64.md
 * §2.3 in miniature -- the seam was already there, it just had one
 * implementation. */

void serial_init(void)               { pl011_init(); }
void serial_write_char(char c)       { pl011_putc(c); }
void serial_write_string(const char *s) { pl011_puts(s); }
void serial_write_hex(uint64_t v)    { pl011_puthex64(v); }
int  serial_has_char(void)           { return pl011_has_char(); }
void     serial_irq_drain(void)      { pl011_irq_drain(); }
void     serial_irq_enable(void)     { pl011_irq_enable(); }
uint32_t serial_rx_dropped(void)     { return pl011_rx_dropped(); }
char serial_read_char(void)          { return pl011_getc(); }
