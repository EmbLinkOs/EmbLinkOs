#include "arch/x86_64/boot/boot_protocol.h"
#include "mm/pmm.h"                  /* KP2V */
#include "drivers/char/serial.h"
#include "include/kprintf.h"

static struct boot_protocol g_bp;
static bool                 g_valid;

static void boot_fatal(const char *why)
{
    /* Serial only, deliberately: this runs before the framebuffer exists,
     * and under UEFI it will also run after ExitBootServices, where no
     * firmware output service is callable. outb to 0x3F8 works in every
     * one of those worlds. */
    serial_write_string("\nFATAL: boot protocol -- ");
    serial_write_string(why);
    serial_write_string("\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void boot_protocol_capture(uint64_t phys)
{
    if (!phys)
        boot_fatal("loader passed a null record pointer in RDI");

    const struct boot_protocol *src = (const struct boot_protocol *)KP2V(phys);

    if (src->magic != BOOT_PROTOCOL_MAGIC)
        boot_fatal("bad magic (loader too old, or RDI clobbered)");
    if (src->version != BOOT_PROTOCOL_VERSION)
        boot_fatal("version mismatch between loader and kernel");
    if (src->size < sizeof(struct boot_protocol))
        boot_fatal("record shorter than this kernel expects");
    if (src->mmap_count == 0)
        boot_fatal("empty memory map");
    if (src->mmap_stride < BOOT_MMAP_STRIDE_MIN)
        boot_fatal("mmap stride smaller than one entry");

    /* Copy the header into .bss. The header is then ours forever; the entry
     * ARRAY is not copied and stays subject to the ordering constraint in
     * the header comment. */
    g_bp    = *src;
    g_valid = true;
}

const struct boot_protocol *boot_protocol_get(void)
{
    return g_valid ? &g_bp : 0;
}

uint32_t boot_mmap_count(void)
{
    return g_valid ? g_bp.mmap_count : 0;
}

const struct boot_mmap_entry *boot_mmap_at(uint32_t index)
{
    if (!g_valid || index >= g_bp.mmap_count)
        return 0;
    /* Stride arithmetic on a byte pointer -- NOT array indexing. */
    const uint8_t *base = (const uint8_t *)KP2V(g_bp.mmap_phys);
    return (const struct boot_mmap_entry *)(base + (uint64_t)index * g_bp.mmap_stride);
}

bool bootinfo_boot_disk_sig(uint32_t *out)
{
    if (!g_valid || g_bp.boot_disk_sig == 0)
        return false;
    if (out)
        *out = g_bp.boot_disk_sig;
    return true;
}

uint8_t bootinfo_boot_drive(void)
{
    return g_valid ? g_bp.boot_drive : 0xFF;
}

void boot_protocol_dump(void)
{
    if (!g_valid) {
        kprintf("bootproto: invalid\n");
        return;
    }
    kprintf("bootproto: v%u size %u fw %s\n",
            (unsigned)g_bp.version, (unsigned)g_bp.size,
            g_bp.firmware == BOOT_FW_UEFI ? "UEFI" : "BIOS");
    kprintf("bootproto: mmap %u entries @ %lx stride %u\n",
            (unsigned)g_bp.mmap_count, (unsigned long)g_bp.mmap_phys,
            (unsigned)g_bp.mmap_stride);
    kprintf("bootproto: fb %lx %ux%u pitch %u bpp %u fmt %u\n",
            (unsigned long)g_bp.fb_addr, (unsigned)g_bp.fb_width,
            (unsigned)g_bp.fb_height, (unsigned)g_bp.fb_pitch,
            (unsigned)g_bp.fb_bpp, (unsigned)g_bp.fb_format);
    kprintf("bootproto: boot drive 0x%X, MBR disk sig 0x%X\n",
            (unsigned)g_bp.boot_drive, (unsigned)g_bp.boot_disk_sig);
}