/* kernel/drivers/video/rawcon.h -- text straight onto the firmware's
 * framebuffer, for the two moments nothing else can draw: before the real
 * console exists, and after the kernel has faulted. See rawcon.c. */
#ifndef _EMBK_RAWCON_H_
#define _EMBK_RAWCON_H_
#include <stdint.h>
#include "include/types.h"

/* Map the boot framebuffer and start carrying kernel log output on it. Needs
 * the VMM; harmless (and silent) on a machine with no framebuffer. */
void rawcon_init(void);

/* The framebuffer driver's mapping, after it has (maybe) changed the mode. */
void rawcon_adopt(uint8_t *front, uint32_t width, uint32_t height,
                  uint32_t pitch, uint32_t bpp, uint32_t format);

void rawcon_putc(char c);
void rawcon_puts(const char *s);
void rawcon_hex(uint64_t v);

/* THE KERNEL HAS FAULTED. From here the screen belongs to this file: it is
 * cleared, a header is drawn, and every other path that would present a frame
 * to the display is refused (rawcon_panicked()), because on a multi-core
 * machine the compositor on another core would otherwise paint the desktop
 * back over the one message anyone will see. Takes no lock, allocates
 * nothing. */
void rawcon_panic_begin(const char *headline);
bool rawcon_panicked(void);

/* How much room is left under the cursor, so a caller can choose how much of
 * a log to show rather than have the bottom of the screen cut it off. */
uint32_t rawcon_rows_left(void);
uint32_t rawcon_cols(void);

#endif
