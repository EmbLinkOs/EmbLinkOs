#ifndef _EMBK_BOOTLOG_H_
#define _EMBK_BOOTLOG_H_

/* Leave the kernel log in a file on the FAT (EFI) partition, so a machine with
 * no serial port, no keyboard and no pointer can still say what happened: the
 * stick goes into another computer and the file is read there. See bootlog.c. */
void bootlog_init(void);

#endif
