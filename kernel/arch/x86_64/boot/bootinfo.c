#include "arch/x86_64/boot/bootinfo.h"
#include "mm/pmm.h"            /* KP2V */
#include "include/kprintf.h"

/* On-the-wire layout of the record stage2 writes at BOOTINFO_PHYS. Packed and
 * fixed: stage2 (boot/stage2/stage2.asm) stores the same three fields at the
 * same offsets by hand. Keep the two in lockstep. */
struct bootinfo_raw {
    uint32_t magic;        /* +0  BOOTINFO_MAGIC when valid            */
    uint32_t disk_sig;     /* +4  MBR disk signature (boot sector 0x1B8) */
    uint8_t  boot_drive;   /* +8  BIOS DL                              */
} __attribute__((packed));

static bool     g_valid;
static uint32_t g_disk_sig;
static uint8_t  g_boot_drive = 0xFF;

void bootinfo_capture(void)
{
    /* Same access pattern as pmm.c reading the e820 map: a low physical address
     * through the direct/higher-half map. Must run before any allocator can
     * reuse the frame. */
    volatile struct bootinfo_raw *bi =
        (volatile struct bootinfo_raw *)KP2V(BOOTINFO_PHYS);

    if (bi->magic != BOOTINFO_MAGIC) {
        /* No record (e.g. an older stage2, or a loader that didn't write it).
         * Not an error -- the kernel just falls back to its default choices. */
        g_valid = false;
        return;
    }
    g_valid      = true;
    g_disk_sig   = bi->disk_sig;
    g_boot_drive = bi->boot_drive;
    kprintf("bootinfo: boot drive 0x%X, MBR disk sig 0x%X\n",
            (unsigned)g_boot_drive, (unsigned)g_disk_sig);
}

bool bootinfo_boot_disk_sig(uint32_t *out)
{
    if (!g_valid || g_disk_sig == 0)
        return false;
    if (out)
        *out = g_disk_sig;
    return true;
}

uint8_t bootinfo_boot_drive(void)
{
    return g_boot_drive;
}
