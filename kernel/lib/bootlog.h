#ifndef _EMBK_BOOTLOG_H_
#define _EMBK_BOOTLOG_H_
#include <stdint.h>

/* Leave the kernel log in a file on the FAT (EFI) partition, so a machine with
 * no serial port, no keyboard and no pointer can still say what happened: the
 * stick goes into another computer and the file is read there. See bootlog.c. */
void bootlog_init(void);

/* Leave one more file beside the log: `data` (which the caller gives up and
 * must keep alive) is written once to `path` on the same boot volume, as soon
 * as that volume is mounted, and retried until the write succeeds. For
 * evidence gathered before any filesystem exists -- the display capture. */
void bootlog_attach(const char *path, const void *data, uint32_t len);

#endif
