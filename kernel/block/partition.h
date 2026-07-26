#ifndef _PARTITION_H_
#define _PARTITION_H_

#include "block/block.h"

// Partition support: an MBR (DOS) partition table parser that exposes each
// primary partition as its own block device. A partition device delegates
// reads/writes to its parent disk, offset by the partition's starting LBA and
// bounded by its length — so a filesystem mounted on "sda1" can never address
// sectors outside that partition.
//
// Naming follows the Linux convention: the N-th partition of disk "sda" is
// "sda1", "sda2", ... (1-based).

// Scan one whole-disk device for an MBR partition table and register a child
// block device for each non-empty primary partition. Returns the number of
// partitions registered (0 if the disk has no valid MBR), or a negative
// EMBK_E* code on a read error.
int embk_partition_scan(struct embk_block_device *disk);

// Scan every whole-disk device currently in the block registry. Call this once,
// after all drivers have registered their disks, before mounting filesystems.
// Newly created partition devices are appended to the registry; this only scans
// the disks that existed when it was called (it never recurses into partitions).
void embk_partition_scan_all(void);

// If `dev` is a partition device this returns its parent whole disk; otherwise
// it returns `dev` itself. Lets a filesystem that mounted a partition reach the
// disk's own sector 0 (e.g. to read the MBR disk signature) without knowing
// whether it mounted a partition or a bare disk.
struct embk_block_device *embk_partition_parent(struct embk_block_device *dev);

// True if `disk` has at least one registered partition (i.e. sector 0 is an MBR,
// not a bare filesystem). A whole-disk filesystem probe must SKIP such a disk:
// its filesystems live in the partitions, and probing the whole disk can match a
// partition's tail structures at the wrong offset -- and, worse, trigger a
// self-heal WRITE over the MBR / boot code. Call after embk_partition_scan_all().
bool embk_block_is_partitioned(struct embk_block_device *disk);

#endif /* _PARTITION_H_ */
