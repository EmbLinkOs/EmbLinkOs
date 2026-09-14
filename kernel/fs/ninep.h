/* kernel/fs/ninep.h -- the host's filesystem, mounted in the guest.
 *
 * READ-ONLY. The value is entirely in reading -- the host builds, the guest
 * runs, and a one-line change is testable without rebuilding a disk image.
 * Writing back into the host's tree from a guest being actively debugged is a
 * way to lose work; the protocol side of it is recorded in docs/TODO.md.
 */
#ifndef _EMBK_NINEP_H_
#define _EMBK_NINEP_H_

#include <stdint.h>
#include "include/types.h"

/* Find a virtio-9p device, negotiate, attach, and mount it at `mount_at`.
 * False means there is none, which is the normal case. */
bool ninep_init(const char *mount_at);
bool ninep_present(void);

/* What the host called the share. */
const char *ninep_tag(void);

/* The ops-table address, for vfs.c's mount-type classifier. */
const void *ninep_vfs_ops_ptr(void);

#endif
