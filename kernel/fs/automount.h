/* kernel/fs/automount.h -- mounting a medium that arrived after boot.
 *
 * Takes a BLOCK DEVICE, not a USB device: a stick, a card reader, an external
 * drive, an image attached at runtime -- they all arrive the same way and
 * should all end up in the same place. See automount.c for where things land
 * and why the volume's own label is not used for it.
 */
#ifndef _EMBK_AUTOMOUNT_H_
#define _EMBK_AUTOMOUNT_H_

#include <stdint.h>
#include "include/types.h"

struct embk_block_device;

/* A medium appeared. Scans it for partitions, offers each candidate to FAT32
 * then EMBKFS, and mounts what takes under /media/<device name>. Returns how
 * many filesystems were mounted -- zero is not an error, it is a stick this
 * kernel cannot read. */
int automount_attach(struct embk_block_device *disk);

/* The medium left. Unmounts everything that came from it, including its
 * partitions. Returns how many mounts went away. */
int automount_detach(struct embk_block_device *disk);

/* What is mounted right now, for `test usbhotplug` and for anything that wants
 * to show the user their removable volumes. */
uint32_t automount_count(void);
bool automount_at(uint32_t idx, const char **at, const char **dev,
                  const char **fs);

#endif
