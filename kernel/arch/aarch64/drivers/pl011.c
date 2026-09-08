#include "arch/aarch64/drivers/pl011.h"
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

int pl011_has_char(void) {
    return (mmio_r32(UARTFR) & FR_RXFE) == 0;
}

char pl011_getc(void) {
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
char serial_read_char(void)          { return pl011_getc(); }
