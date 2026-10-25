/* kernel/lib/bootlog.c -- leave the boot log on the stick.
 *
 * WHY THIS EXISTS. An HP ProBook runs this kernel, mounts its root filesystem
 * off a USB stick, and puts a desktop on the screen. It also has no serial
 * port, and its keyboard and mouse -- which work in the firmware -- do not
 * work once this kernel owns the USB controller. So the machine has no way to
 * answer a question: nothing to type on, nothing to click, and a screen the
 * desktop has taken over. GUI+K exists for exactly this and needs a keyboard.
 *
 * A FILE ON THE EFI PARTITION NEEDS NOTHING. The stick is already mounted, the
 * partition is FAT32, and every computer in the world can read it: power the
 * machine off, move the stick, open the file. That is the whole idea.
 *
 * Written on a timer rather than once, because the interesting failure may not
 * have happened yet when the desktop should have appeared -- and rewritten in
 * place each time, so the file is always the newest 64 KB of the log rather
 * than a pile of fragments.
 */
#include "lib/bootlog.h"
#include "lib/klog.h"
#include "fs/automount.h"
#include "fs/fat32.h"
#include "mm/kheap.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "process/process.h"
#include "process/sched.h"
#include "drivers/timer/timer.h"
#include "include/spinlock.h"
#include "include/types.h"   /* bool -- NOT <stdbool.h>: types.h typedefs it */

#define BOOTLOG_PATH      "/EMBLINK.LOG"
#define BOOTLOG_FIRST_MS   4000    /* soon: a machine that wedges at 15 s must still leave a log */
#define BOOTLOG_EVERY_MS   5000

/* WHICH VOLUME? THE ONE THIS KERNEL BOOTED FROM, AND NO OTHER.
 *
 * "The first mounted FAT32 volume" is not that. Plug a stick into a running
 * EmbLinkOS and it is mounted within half a second, and if it happens to take
 * the first slot the boot log lands on SOMEONE ELSE'S STICK -- 64 KB of kernel
 * log written, unasked, onto whatever a person plugged in. It also breaks the
 * stick for whoever was using it: the write goes out over the same USB pipe
 * their read is using (tools/usb_hotplug.py caught exactly this, as an OHCI
 * bulk timeout and a volume that "is mounted but cannot be listed").
 *
 * The boot volume is recognised, not guessed: it is the one carrying the
 * loader that started this kernel. Nothing else on a person's desk has
 * /EFI/BOOT/BOOTX64.EFI on it, and if something does, it is a boot stick and
 * writing a log to it is what this file is for. Pinned once found, so a later
 * plug cannot change the answer.
 */
static struct fat32_volume *bootlog_volume(void)
{
    static struct fat32_volume *pinned;
    if (pinned) return pinned;
    for (int i = 0; ; i++) {
        struct fat32_volume *v = automount_fat32_volume(i);
        if (!v) return NULL;
        if (fat32_exists(v, "/EFI/BOOT/BOOTX64.EFI") ||
            fat32_exists(v, "/EFI/BOOT/BOOTAA64.EFI")) {
            pinned = v;
            return v;
        }
    }
}

/* Files handed over before the volume existed (bootlog_attach). Registered
 * during boot, written by the bootlog thread, so the lock only ever meets a
 * late registration racing the first write. */
#define BOOTLOG_ATTACH_MAX 4
static struct {
    const char *path;
    const void *data;
    uint32_t len;
    bool written;
} g_attached[BOOTLOG_ATTACH_MAX];
static unsigned g_attached_count;
static spinlock_t g_attach_lock = SPINLOCK_INIT;

void bootlog_attach(const char *path, const void *data, uint32_t len)
{
    spin_lock(&g_attach_lock);
    bool kept = g_attached_count < BOOTLOG_ATTACH_MAX;
    if (kept) {
        unsigned i = g_attached_count++;
        g_attached[i].path = path;
        g_attached[i].data = data;
        g_attached[i].len = len;
        g_attached[i].written = false;
    }
    spin_unlock(&g_attach_lock);
    if (!kept)
        kprintf("bootlog: no room to attach %s; it will not reach the stick\n", path);
}

static void bootlog_write_attached(struct fat32_volume *vol)
{
    for (unsigned i = 0; i < BOOTLOG_ATTACH_MAX; i++) {
        spin_lock(&g_attach_lock);
        bool pending = i < g_attached_count && !g_attached[i].written;
        const char *path = pending ? g_attached[i].path : 0;
        const void *data = pending ? g_attached[i].data : 0;
        uint32_t len = pending ? g_attached[i].len : 0;
        spin_unlock(&g_attach_lock);
        if (!pending) continue;
        int rc = fat32_write(vol, path, (const uint8_t *)data, len);
        if (rc < 0) {
            kprintf("bootlog: could not write %s: %s (will retry)\n", path, embk_strerror(rc));
            continue;
        }
        spin_lock(&g_attach_lock);
        g_attached[i].written = true;
        spin_unlock(&g_attach_lock);
        kprintf("bootlog: wrote %s to the EFI partition (%u bytes)\n", path, (unsigned)len);
    }
}

static void bootlog_write_once(void)
{
    struct fat32_volume *vol = bootlog_volume();
    if (!vol) return;                    /* not booted from a FAT volume: nothing to do */
    bootlog_write_attached(vol);

    /* The heap, not .bss: the kernel image has to fit under the address the
     * firmware loads it at, and 64 KB of it for a copy of something already in
     * memory is a sixteenth of the budget (tools/kernel_fits.py). */
    uint32_t cap = 65536;
    char *buf = (char *)kmalloc(cap);
    if (!buf) return;

    uint32_t n = klog_tail(buf, cap);
    if (n) {
        /* fat32_write returns the byte count, not a status: anything negative
         * is the failure. Reporting "FAILED" on a successful write of 11673
         * bytes is the kind of wrong that sends someone looking in the wrong
         * place, which is the opposite of what this file is for. */
        int rc = fat32_write(vol, BOOTLOG_PATH, (const uint8_t *)buf, n);
        static bool said;
        if (!said) {
            said = true;
            if (rc < 0)
                kprintf("bootlog: could not write %s: %s\n", BOOTLOG_PATH,
                        embk_strerror(rc));
            else
                kprintf("bootlog: the kernel log is on the EFI partition as %s "
                        "(%u bytes, rewritten every %u s)\n",
                        BOOTLOG_PATH, (unsigned)n, BOOTLOG_EVERY_MS / 1000U);
        }
    }
    kfree(buf);
}

static void bootlog_thread(void)
{
    uint64_t next = timer_uptime_ms() + BOOTLOG_FIRST_MS;
    for (;;) {
        if (timer_uptime_ms() >= next) {
            bootlog_write_once();
            next = timer_uptime_ms() + BOOTLOG_EVERY_MS;
        }
        sched_sleep_ms(1000);
    }
}

void bootlog_init(void)
{
    process_create_kthread(bootlog_thread, NULL);
    kprintf("bootlog: the kernel log will be left on the EFI partition as %s\n",
            BOOTLOG_PATH);
}
