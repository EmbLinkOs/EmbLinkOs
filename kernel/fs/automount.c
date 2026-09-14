/* kernel/fs/automount.c -- a medium appeared; make it reachable.
 *
 * WHAT THIS IS FOR. Until now every filesystem this kernel mounted was found
 * once, during boot, by code that knew where to look. That is the right shape
 * for the root filesystem and the wrong shape for everything else: a USB stick
 * pushed into a running machine is not a boot-time fact, and the whole point
 * of the port is that you can do it whenever you like.
 *
 * DELIBERATELY NOT USB-SPECIFIC. It takes a block device, not a USB device.
 * A card reader, an external drive over a controller this kernel does not have
 * yet, an image attached at runtime -- they all arrive as "a block device
 * exists now" and they should all end up in the same place. The USB layer
 * calls in here; nothing here knows what USB is.
 *
 * WHERE THINGS LAND: /media/<device name>, so a stick whose first partition is
 * sdc1 appears at /media/sdc1. Not a friendly volume label, and that is on
 * purpose for now -- a label is attacker-controlled text from an untrusted
 * medium, and putting it straight into a path is how you get a "volume" called
 * `../../system`. The name the kernel assigned is the one thing about a
 * removable disk that is certainly safe.
 *
 * WHAT IS TRIED, in order: every partition the medium declares, then the whole
 * device if it declares none (a stick formatted without a partition table,
 * which is common and legal). Each candidate is offered to FAT32 first and
 * EMBKFS second -- FAT32 because that is what a stick from anywhere else in
 * the world has on it.
 */
#include <stdint.h>
#include <stddef.h>

#include "include/types.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/errno.h"
#include "include/kmalloc.h"
#include "block/block.h"
#include "block/partition.h"
#include "fs/vfs.h"
#include "fs/fat32.h"
#include "fs/embkfs/embkfs.h"
#include "fs/iso9660.h"
#include "fs/automount.h"

/* One mounted removable volume. The volume structs are big and there are few
 * of these, so they are allocated rather than held as a static array. */
struct auto_mount {
    bool used;
    struct embk_block_device *dev;      /* the partition or whole disk    */
    struct embk_block_device *parent;   /* the medium it belongs to       */
    char   at[64];
    void  *vol;                          /* fat32_volume* or embkfs_volume* */
    bool   is_fat;
    bool   is_iso;                       /* the volume is on the disc itself */
};

#define AUTO_MAX 8
static struct auto_mount g_auto[AUTO_MAX];

static void path_for(const char *name, char *out, uint32_t cap) {
    uint32_t i = 0;
    const char *pfx = "/media/";
    while (*pfx && i < cap - 1) out[i++] = *pfx++;
    while (*name && i < cap - 1) out[i++] = *name++;
    out[i] = 0;
}

/* Offer one block device to each filesystem in turn. */
static bool try_mount_one(struct embk_block_device *dev,
                          struct embk_block_device *parent) {
    struct auto_mount *slot = NULL;
    for (int i = 0; i < AUTO_MAX; i++)
        if (!g_auto[i].used) { slot = &g_auto[i]; break; }
    if (!slot) {
        kprintf("automount: no free slot for %s\n", dev->name);
        return false;
    }

    char at[64];
    path_for(dev->name, at, sizeof at);

    /* ISO 9660 FIRST WHEN THE BLOCK IS 2048, because a disc is never
     * anything else and probing it as FAT32 reads a boot sector that is not
     * there. The check is the medium's geometry, not a device type: an ISO
     * image written to a USB stick is still an ISO. */
    if (iso9660_probe(dev)) {
        if (iso9660_mount(dev, at) == EMBK_OK) {
            slot->used = true; slot->dev = dev; slot->parent = parent;
            slot->vol = NULL; slot->is_iso = true;
            memcpy(slot->at, at, sizeof at);
            return true;
        }
    }

    /* FAT32 next, because that is what a stick formatted on any other
     * operating system carries. */
    struct fat32_volume *fv = kmalloc(sizeof *fv);
    if (fv) {
        memset(fv, 0, sizeof *fv);
        if (fat32_mount(dev, fv) == EMBK_OK) {
            if (fat32_vfs_register(at, fv) == EMBK_OK) {
                slot->used = true; slot->dev = dev; slot->parent = parent;
                slot->vol = fv;   slot->is_fat = true;
                memcpy(slot->at, at, sizeof at);
                kprintf("automount: %s is FAT32 -- mounted at %s\n", dev->name, at);
                return true;
            }
        }
        kfree(fv);
    }

    struct embkfs_volume *ev = kmalloc(sizeof *ev);
    if (ev) {
        memset(ev, 0, sizeof *ev);
        if (embkfs_mount(dev, ev) == EMBK_OK) {
            if (embkfs_vfs_register(at, ev) == EMBK_OK) {
                slot->used = true; slot->dev = dev; slot->parent = parent;
                slot->vol = ev;   slot->is_fat = false;
                memcpy(slot->at, at, sizeof at);
                kprintf("automount: %s is EMBKFS -- mounted at %s\n", dev->name, at);
                return true;
            }
        }
        kfree(ev);
    }
    return false;
}

int automount_attach(struct embk_block_device *disk) {
    if (!disk) return -EMBK_EINVAL;

    /* PARTITIONS FIRST. The scan registers each one as its own block device,
     * so after this the table has sdc1, sdc2 ... and those are what get
     * mounted -- mounting the whole disk over a partition table finds
     * nothing, which reads as "the stick is empty". */
    int parts = embk_partition_scan(disk);
    int mounted = 0;

    if (parts > 0) {
        /* Walk the table rather than remembering indices: the scan appends,
         * and a partition is recognisable by having this disk as its parent. */
        for (uint32_t i = 0; i < embk_block_count(); i++) {
            struct embk_block_device *d = embk_block_get(i);
            /* A PARTITION OF THIS DISK, which is not the same test as "has
             * this disk as a parent": embk_partition_parent returns the
             * device itself for a whole disk, so the disk would match its own
             * filter and be mounted a second time alongside its partitions. */
            if (!d || d == disk || embk_partition_parent(d) != disk) continue;
            if (try_mount_one(d, disk)) mounted++;
        }
    }

    /* NO PARTITION TABLE IS NOT AN ERROR. A stick formatted as one big FAT32
     * with no MBR is ordinary, and refusing it would make half the sticks in
     * the world look broken. */
    if (mounted == 0 && try_mount_one(disk, disk)) mounted++;

    if (mounted == 0)
        kprintf("automount: %s has no filesystem this kernel can read\n",
                disk->name);
    return mounted;
}

/* The medium left. Unmount everything that came from it, whether that is the
 * disk itself or partitions of it. */
int automount_detach(struct embk_block_device *disk) {
    int n = 0;
    for (int i = 0; i < AUTO_MAX; i++) {
        if (!g_auto[i].used) continue;
        if (g_auto[i].dev != disk && g_auto[i].parent != disk) continue;

        vfs_unmount(g_auto[i].at);
        kprintf("automount: %s is gone -- %s unmounted\n",
                g_auto[i].dev->name, g_auto[i].at);
        /* The volume struct is ours; the block device behind it belongs to
         * whoever registered it and is unregistered by them. */
        kfree(g_auto[i].vol);
        memset(&g_auto[i], 0, sizeof g_auto[i]);
        n++;
    }
    return n;
}

uint32_t automount_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < AUTO_MAX; i++) if (g_auto[i].used) n++;
    return n;
}

bool automount_at(uint32_t idx, const char **at, const char **dev,
                  const char **fs) {
    uint32_t seen = 0;
    for (int i = 0; i < AUTO_MAX; i++) {
        if (!g_auto[i].used) continue;
        if (seen++ != idx) continue;
        if (at)  *at  = g_auto[i].at;
        if (dev) *dev = g_auto[i].dev->name;
        if (fs)  *fs  = g_auto[i].is_iso ? "iso9660"
                      : g_auto[i].is_fat ? "fat32" : "embkfs";
        return true;
    }
    return false;
}
