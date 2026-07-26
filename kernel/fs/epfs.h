#ifndef __EPFS_H__
#define __EPFS_H__

/* kernel/fs/epfs.h -- a small RAM-backed filesystem that holds nothing but
 * VFS_DT_ENDPOINT nodes (and the root directory to hold them), mounted at
 * /run. EmbLink UI Piece 1, Layer B: this is what makes `chan_listen("/run/
 * compositor")` a real VFS path a client can `chan_connect` to by name.
 *
 * Deliberately NOT the on-disk EMBKFS: endpoints are ephemeral (gone on
 * reboot, gone when their owning process dies) and have no business on a
 * persistent COW filesystem. This is a flat namespace directly under root
 * (no subdirectories) -- the spec's own usage (`/run/compositor`) never
 * needs nesting; mkdir under /run is deferred (returns -EMBK_ENOSYS). */

#include "fs/vfs.h"

#define EPFS_ROOT_INO 1

void epfs_init(void);

/* Mount epfs at `path` (expected: "/run"). */
int  epfs_vfs_register(const char *path);

/* ---- EmbDBG v2 IPC Explorer: published-endpoint snapshot -----------------
 * Every live listening endpoint is registered as an epfs node; this lists them
 * with their name + endpoint object pointer, so the IPC view can show published
 * endpoints even before any accept handle exists. */
struct epfs_ep_info {
    char     name[32];   /* the node name under the epfs mount (e.g. "compositor") */
    uint64_t endpoint;   /* struct listen_endpoint* */
};
int epfs_endpoints_snapshot(struct epfs_ep_info *out, int max);

#endif /* __EPFS_H__ */
