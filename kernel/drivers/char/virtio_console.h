/* A serial port that is not a 16550 -- a queue instead of one byte per I/O
 * port write, and somewhere for a console to live on a machine with no serial
 * header. See the .c. */
#ifndef _EMBK_VIRTIO_CONSOLE_H_
#define _EMBK_VIRTIO_CONSOLE_H_
#include <stdint.h>
#include "include/types.h"
bool     virtio_console_init(void);
void     virtio_console_write(const char *s, uint32_t len);
bool     virtio_console_present(void);
uint64_t virtio_console_bytes(void);
#endif
