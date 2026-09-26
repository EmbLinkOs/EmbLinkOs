/* kernel/drivers/video/rawcon.c -- text straight onto the firmware's
 * framebuffer.
 *
 * WHY THIS EXISTS. A laptop has no serial port. Until this file, the kernel's
 * log reached a screen only from console_init() onwards -- and console_init()
 * runs AFTER PCI enumeration, both IOMMUs, the TPM, the error-record store,
 * audio, SMBus, every USB controller, AHCI and NVMe. A real machine that hangs
 * in any of those shows the loader's last line, "exiting boot services +
 * jumping to kernel...", and nothing else, forever. A kernel fault was worse:
 * the register dump went ONLY to the serial port, so the screen froze on
 * whatever the desktop had last drawn.
 *
 * So there are two jobs and one mechanism:
 *
 *   * EARLY: from the moment the VMM can map the framebuffer, every kernel log
 *     line is drawn here too, until console_init() takes over. A hang then
 *     leaves its last line on the screen, which a person can photograph.
 *   * PANIC: rawcon_panic_begin() takes the screen back from everything,
 *     including a desktop that has owned it for an hour.
 *
 * WHAT IT MAY NOT DO, and each of these is a requirement: take a lock (the
 * faulting context may hold it), allocate (the heap may be what broke), or
 * depend on the graphics stack (it may not exist yet, or be the fault). It
 * writes pixels into the linear framebuffer the firmware set up, through one
 * mapping made at init, and that is all.
 *
 * It does not scroll. Scrolling a 4K framebuffer by memmove is a second of
 * work per line on a slow machine, and a boot log that takes minutes to print
 * is a different bug. When it reaches the bottom it starts again at the top,
 * clearing each line as it writes it and marking the newest with a bar, so
 * the screen always reads as the last N lines with an obvious "now". */
#include "drivers/video/rawcon.h"
#include "drivers/video/font_8x16.h"
#include "boot/boot_protocol.h"
#include "mm/vmm.h"
#include "include/kprintf.h"
#include "drivers/char/serial.h"
#include "lib/klog.h"

static uint32_t *g_px;               /* the mapping; 0 = no framebuffer */
static uint32_t  g_w, g_h, g_stride; /* stride in PIXELS */
static bool      g_bgr;
static uint32_t  g_scale = 1;
static uint32_t  g_cols, g_rows;
static uint32_t  g_col, g_row;
static bool      g_active;           /* carrying the kernel log */
static volatile bool g_panicked;

#define CELL_W (FONT_WIDTH * g_scale)
#define CELL_H (FONT_HEIGHT * g_scale)

static uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return g_bgr ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | b
                 : ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t g_fg, g_bg, g_mark;

static void fill_row(uint32_t row, uint32_t colour) {
    uint32_t y0 = row * CELL_H;
    for (uint32_t y = y0; y < y0 + CELL_H && y < g_h; y++) {
        uint32_t *line = g_px + (uint64_t)y * g_stride;
        for (uint32_t x = 0; x < g_w; x++) line[x] = colour;
    }
}

static void mark_row(uint32_t row, uint32_t colour) {
    uint32_t y0 = row * CELL_H;
    for (uint32_t y = y0; y < y0 + CELL_H && y < g_h; y++) {
        uint32_t *line = g_px + (uint64_t)y * g_stride;
        for (uint32_t x = 0; x < 3 * g_scale && x < g_w; x++) line[x] = colour;
    }
}

static void glyph(uint32_t col, uint32_t row, unsigned char c) {
    const uint8_t *gl = &font_8x16[(uint32_t)c * FONT_HEIGHT];
    uint32_t x0 = col * CELL_W + 4 * g_scale, y0 = row * CELL_H;
    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = gl[gy];
        for (uint32_t sy = 0; sy < g_scale; sy++) {
            uint32_t y = y0 + gy * g_scale + sy;
            if (y >= g_h) return;
            uint32_t *line = g_px + (uint64_t)y * g_stride;
            for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
                uint32_t v = (bits & (0x80u >> gx)) ? g_fg : g_bg;
                for (uint32_t sx = 0; sx < g_scale; sx++) {
                    uint32_t x = x0 + gx * g_scale + sx;
                    if (x < g_w) line[x] = v;
                }
            }
        }
    }
}

static bool g_screen_full;             /* panic mode: never wrap over the header */

static void new_line(void) {
    if (g_panicked) {
        if (g_row + 1 >= g_rows) { g_screen_full = true; return; }
        g_col = 0; g_row++;
        return;
    }
    mark_row(g_row, g_bg);                 /* the old "now" is not now any more */
    g_col = 0;
    if (++g_row >= g_rows) g_row = 0;
    fill_row(g_row, g_bg);                 /* write on a clean line... */
    if (g_row + 1 < g_rows) fill_row(g_row + 1, g_bg);   /* ...with a gap after it */
    mark_row(g_row, g_mark);
}

static void draw_char(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c == '\n') { new_line(); return; }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\t') { do rawcon_putc(' '); while (g_col % 8); return; }
    if (c < 32 && c != 0) return;
    if (g_col >= g_cols) new_line();
    glyph(g_col++, g_row, c);
}

void rawcon_putc(char ch) {
    /* SERIAL FIRST, as console_putchar() does: a registered secondary sink
     * REPLACES kprintf's own serial write rather than adding to it, so a
     * secondary that forgets this silently takes the kernel log off the serial
     * port. The first version did, and every line from the heap to the real
     * console -- ACPI, the TSC's frequency, PCI -- vanished from it. */
    if (g_active && !g_panicked) serial_write_char(ch);
    if (!g_px || (!g_active && !g_panicked) || g_screen_full) return;
    draw_char(ch);
}

/* WHAT WAS SAID BEFORE THERE WAS A SCREEN. rawcon_init runs once the VMM can
 * map the framebuffer, which is after the banner, the memory map and the
 * physical allocator have all printed -- exactly the stretch a machine with no
 * serial port (a laptop; a Raspberry Pi on a television) is most likely to
 * hang in on its first boot. kprintf has been keeping it in the klog ring all
 * along, so draw the part that fits on one screen: the newest rows-2 lines,
 * so the replay never wraps over itself. Drawn only -- serial already has it. */
static char g_replay[8192];

static void replay_early_log(void) {
    uint32_t n = klog_tail(g_replay, sizeof g_replay);
    if (!n || g_rows < 3) return;

    /* Walk back a LINE at a time, charging each the screen ROWS it takes: a
     * line longer than the screen is wide wraps, and counting newlines alone
     * let the replay run past the bottom and over its own first lines. */
    uint32_t budget = g_rows - 2, used = 0, start = n;
    while (start > 0) {
        uint32_t end = start, b = start;
        while (b > 0 && g_replay[b - 1] != '\n') b--;
        uint32_t len = end - b;
        uint32_t rows = len ? (len + g_cols - 1) / g_cols : 1;
        if (used + rows > budget) break;
        used += rows;
        start = b > 0 ? b - 1 : 0;          /* step over the newline before it */
        if (b == 0) break;
    }
    /* `start` is on a newline (or 0); begin just after it. */
    if (start > 0 || g_replay[0] == '\n') start++;
    for (uint32_t i = start; i < n; i++) draw_char(g_replay[i]);
}

void rawcon_puts(const char *s) {
    while (s && *s) rawcon_putc(*s++);
}

void rawcon_hex(uint64_t v) {
    static const char d[] = "0123456789abcdef";
    rawcon_puts("0x");
    for (int i = 60; i >= 0; i -= 4) rawcon_putc(d[(v >> i) & 0xF]);
}

static bool rawcon_ready(void) { return g_active && g_px; }

void rawcon_init(void) {
    const struct boot_protocol *bp = boot_protocol_get();
    if (!bp || !bp->fb_addr || bp->fb_bpp != 32 || !bp->fb_width || !bp->fb_height)
        return;                             /* nothing to draw on: say nothing */

    g_w = bp->fb_width;
    g_h = bp->fb_height;
    g_stride = bp->fb_pitch / 4;
    if (g_stride < g_w) g_stride = g_w;
    g_bgr = bp->fb_format != 0;

    uint64_t size = (uint64_t)bp->fb_pitch * g_h;
    uint64_t va = vmm_map_mmio_wc(bp->fb_addr, size);
    if (!va) return;
    g_px = (uint32_t *)(uintptr_t)va;

    /* LEGIBLE IN A PHOTOGRAPH. An 8x16 glyph on a 4K laptop panel is about a
     * millimetre tall, which is technically on the screen and practically not
     * readable by anyone holding a phone over it. */
    g_scale = g_w >= 3000 ? 3 : g_w >= 1900 ? 2 : 1;
    g_cols = (g_w - 8 * g_scale) / CELL_W;
    g_rows = g_h / CELL_H;
    if (!g_cols || !g_rows) { g_px = 0; return; }

    g_fg = pack(0xD8, 0xDC, 0xE4);
    g_bg = pack(0x0B, 0x0D, 0x12);
    g_mark = pack(0x5B, 0x8C, 0xFF);
    for (uint32_t r = 0; r < g_rows; r++) fill_row(r, g_bg);
    g_row = 0; g_col = 0;
    mark_row(0, g_mark);
    replay_early_log();

    g_active = true;
    kprintf_set_secondary(rawcon_ready, rawcon_putc);
    kprintf("rawcon: kernel log on the firmware framebuffer, %ux%u, %ux%u cells\n",
            g_w, g_h, g_cols, g_rows);
}

/* THE DISPLAY CHANGED MODE UNDER US. A graphics driver (QEMU's Bochs adapter,
 * a virtio GPU) may reprogram the scan-out after rawcon_init mapped the
 * firmware's -- and a panic drawn with the old width and stride on the new
 * mode comes out as the same text smeared four times across the screen.
 * Measured, which is why this exists. fb_init() hands over the mapping it
 * made, so nothing new has to be mapped (and nothing can be, at panic time). */
void rawcon_adopt(uint8_t *front, uint32_t width, uint32_t height,
                  uint32_t pitch, uint32_t bpp, uint32_t format) {
    if (!front || bpp != 32 || !width || !height) return;
    g_px = (uint32_t *)(uintptr_t)front;
    g_w = width;
    g_h = height;
    g_stride = pitch / 4;
    if (g_stride < g_w) g_stride = g_w;
    g_bgr = format != 0;
    g_scale = g_w >= 3000 ? 3 : g_w >= 1900 ? 2 : 1;
    g_cols = (g_w - 8 * g_scale) / CELL_W;
    g_rows = g_h / CELL_H;
    g_fg = pack(0xD8, 0xDC, 0xE4);
    g_bg = pack(0x0B, 0x0D, 0x12);
    g_mark = pack(0x5B, 0x8C, 0xFF);
    if (g_row >= g_rows) g_row = 0;
    g_col = 0;
}

bool rawcon_panicked(void) { return g_panicked; }

uint32_t rawcon_rows_left(void) {
    return (g_px && g_rows > g_row + 1) ? g_rows - g_row - 1 : 0;
}
uint32_t rawcon_cols(void) { return g_cols; }

void rawcon_panic_begin(const char *headline) {
    if (!g_px) { g_panicked = true; return; }
    g_panicked = true;                      /* first: every present is refused */
    g_active = false;                       /* and kprintf no longer draws here */

    uint32_t red = pack(0x8A, 0x10, 0x18);
    g_bg = pack(0x10, 0x04, 0x06);
    for (uint32_t r = 0; r < g_rows; r++) fill_row(r, r < 3 ? red : g_bg);

    g_fg = pack(0xFF, 0xFF, 0xFF);
    uint32_t keep_bg = g_bg;
    g_bg = red;
    g_row = 1; g_col = 0;
    rawcon_puts("  EmbLinkOS stopped: ");
    rawcon_puts(headline ? headline : "kernel fault");
    g_bg = keep_bg;
    g_row = 4; g_col = 0;
    g_fg = pack(0xF2, 0xE6, 0xE6);
    g_mark = keep_bg;                       /* no "now" bar on a still picture */
}
