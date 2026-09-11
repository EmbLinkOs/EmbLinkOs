#ifndef _EMBK_NVME_H_
#define _EMBK_NVME_H_

#include "include/types.h"

/* NVM Express over PCIe -- the storage interface of essentially every machine
 * built since about 2016. See nvme.c for the design and its limits.
 *
 * Probes every PCI function of class 01h / subclass 08h / interface 02h,
 * brings each controller up, and registers every active namespace with the
 * block layer (so they appear as the next sdX, after whatever ATA, AHCI and
 * virtio-blk registered first -- the boot disk keeps its name).
 *
 * Returns true if at least one namespace was registered. Safe to call on a
 * machine with no NVMe controller: it says so and returns false. */
bool nvme_init(void);

/* Tell every controller the power is about to go: a NORMAL SHUTDOWN
 * notification (CC.SHN), waited for until the controller reports it complete.
 * A drive that loses power without one does not lose data that was flushed,
 * but it does run its recovery on the next power-up, and some count it as an
 * unsafe shutdown in their health log. Called on the way to power-off. */
void nvme_shutdown_all(void);

/* Bring every controller back after nvme_shutdown_all(), for when the
 * power-off that followed it did not happen. See nvme.c. */
void nvme_restart_all(void);

/* How many namespaces are registered, and each one's block device and largest
 * single transfer -- for the self-test, which has to find an NVMe namespace
 * among all block devices and push transfers across that boundary. */
int nvme_namespace_count(void);
struct embk_block_device;
struct embk_block_device *nvme_namespace_blkdev(int index);
uint32_t nvme_namespace_max_xfer(int index);

/* Shut down and restart the controller that owns namespace `index`, under its
 * lock -- the witness's way of running the power-off recovery path. */
int nvme_cycle_namespace_controller(int index);

/* The witness (nvmetest.c): read, write, verify and flush on a namespace that
 * carries the scratch marker, and refuse to touch one that does not. Returns
 * 0 when every check held, -EMBK_ENOENT when there is no scratch namespace. */
int nvme_selftest_run(void);

#endif
