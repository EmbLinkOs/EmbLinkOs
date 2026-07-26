#ifndef _ARCH_X86_64_BOOT_PROTOCOL_H_
#define _ARCH_X86_64_BOOT_PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>
#include "include/types.h"

/* The single record a loader hands the kernel. Replaces three separate
 * fixed-physical-address conventions (fb info at 0x6000, boot device at
 * 0x6F00, e820 at 0x7000) with one struct whose ADDRESS IS PASSED IN RDI --
 * so where it lives becomes the loader's private business, not a number the
 * kernel has to agree on.
 *
 * Two loaders fill this in: boot/stage2/stage2.asm (BIOS) and, later, the
 * UEFI loader. stage2 stores the fields at these exact offsets BY HAND, so
 * the %defines at the top of stage2.asm and the _Static_asserts at the
 * bottom of this file must stay in lockstep. Same discipline the old
 * bootinfo_raw used, now with the offsets machine-checked instead of
 * asserted in a comment.
 *
 * Growing this struct: add fields at the END, bump BOOT_PROTOCOL_VERSION,
 * and check `size` before reading anything new. Never reorder. */

#define BOOT_PROTOCOL_MAGIC    0x4F52504B4E494C45ULL  /* "ELINKPRO" */
#define BOOT_PROTOCOL_VERSION  1

/* Which firmware built this record. The kernel should need this for almost
 * nothing -- if a subsystem starts branching on it, that is a sign the
 * difference leaked out of the loader where it belongs. */
#define BOOT_FW_BIOS  1
#define BOOT_FW_UEFI  2

/* One memory-map entry. Deliberately the same shape and the same TYPE
 * NUMBERING as the old struct e820_entry (E820_USABLE and friends in pmm.h
 * stay valid) -- under BIOS the entries pass through untouched, and the UEFI
 * loader will translate EFI_MEMORY_DESCRIPTOR into this. The kernel gets one
 * format regardless of firmware. */
struct boot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
};

struct boot_protocol {
    uint64_t magic;          /* +0x00  BOOT_PROTOCOL_MAGIC, written LAST     */
    uint32_t version;        /* +0x08  BOOT_PROTOCOL_VERSION                 */
    uint32_t size;           /* +0x0C  bytes the loader actually filled      */

    uint32_t firmware;       /* +0x10  BOOT_FW_*                             */
    uint32_t mmap_count;     /* +0x14  entries; 0 = no map (fatal)           */
    uint64_t mmap_phys;      /* +0x18  physical addr of entry 0              */
    uint32_t mmap_stride;    /* +0x20  BYTES between entries -- see below    */

    uint32_t boot_disk_sig;  /* +0x24  MBR signature, 0 = unknown            */

    uint64_t fb_addr;        /* +0x28  linear framebuffer physical address   */
    uint32_t fb_width;       /* +0x30  pixels                                */
    uint32_t fb_height;      /* +0x34  pixels                                */
    uint32_t fb_pitch;       /* +0x38  BYTES per scanline (not pixels)       */
    uint32_t fb_bpp;         /* +0x3C  bits per pixel                        */
    uint32_t fb_format;      /* +0x40  FB_FORMAT_RGB / FB_FORMAT_BGR         */

    uint8_t  boot_drive;     /* +0x44  BIOS DL, 0xFF = unknown               */
    uint8_t  _reserved[3];   /* +0x45                                        */

    uint64_t acpi_rsdp;      /* +0x48  ACPI RSDP phys, 0 = unknown (BIOS: the
                              *        kernel falls back to the legacy scan;
                              *        UEFI: from the EFI configuration table) */
};                           /*  size  0x50                                  */

/* Why mmap_stride exists when the loader normalizes every entry to
 * sizeof(struct boot_mmap_entry) anyway: it lets the ENTRY grow later
 * without breaking a kernel built against the old size, exactly as `size`
 * does for the outer struct. Iterate with the stride, never with map[i].
 * (This is also the trap UEFI's GetMemoryMap sets with DescriptorSize --
 * worth having the habit before it matters.) */

#define BOOT_MMAP_STRIDE_MIN  24

_Static_assert(sizeof(struct boot_mmap_entry) == 24, "mmap entry is 24 bytes");
_Static_assert(sizeof(struct boot_protocol)   == 0x50, "boot_protocol is 0x50");
_Static_assert(offsetof(struct boot_protocol, magic)         == 0x00, "off");
_Static_assert(offsetof(struct boot_protocol, version)       == 0x08, "off");
_Static_assert(offsetof(struct boot_protocol, size)          == 0x0C, "off");
_Static_assert(offsetof(struct boot_protocol, firmware)      == 0x10, "off");
_Static_assert(offsetof(struct boot_protocol, mmap_count)    == 0x14, "off");
_Static_assert(offsetof(struct boot_protocol, mmap_phys)     == 0x18, "off");
_Static_assert(offsetof(struct boot_protocol, mmap_stride)   == 0x20, "off");
_Static_assert(offsetof(struct boot_protocol, boot_disk_sig) == 0x24, "off");
_Static_assert(offsetof(struct boot_protocol, fb_addr)       == 0x28, "off");
_Static_assert(offsetof(struct boot_protocol, fb_width)      == 0x30, "off");
_Static_assert(offsetof(struct boot_protocol, fb_height)     == 0x34, "off");
_Static_assert(offsetof(struct boot_protocol, fb_pitch)      == 0x38, "off");
_Static_assert(offsetof(struct boot_protocol, fb_bpp)        == 0x3C, "off");
_Static_assert(offsetof(struct boot_protocol, fb_format)     == 0x40, "off");
_Static_assert(offsetof(struct boot_protocol, boot_drive)    == 0x44, "off");
_Static_assert(offsetof(struct boot_protocol, acpi_rsdp)     == 0x48, "off");

/* Capture the record into a kernel-owned copy. Call ONCE, as the first thing
 * in kernel_main -- before pmm_init, which now reads its map through here.
 * Halts loudly on an invalid record: no memory map means no PMM, and a
 * silent fallback would fail somewhere unrelated and much later. */
void boot_protocol_capture(uint64_t phys);

const struct boot_protocol *boot_protocol_get(void);
void boot_protocol_dump(void);

/* Memory map access.
 *
 * ORDERING CONSTRAINT: the ENTRIES are not copied -- they are still sitting
 * at mmap_phys and are read through KP2V, so this only works before the PMM
 * can hand those frames out. Exactly the constraint the old KP2V(0x7004)
 * read lived under; now it is written down. */
uint32_t boot_mmap_count(void);
const struct boot_mmap_entry *boot_mmap_at(uint32_t index);

/* Boot-device facts (same API the old bootinfo.h exported, so callers of
 * these two need no change). */
bool    bootinfo_boot_disk_sig(uint32_t *out);
uint8_t bootinfo_boot_drive(void);

/* ACPI RSDP physical address the loader found (0 = unknown -> caller scans the
 * legacy BIOS memory areas instead). Set under UEFI, where that scan finds
 * nothing because the RSDP is in the EFI configuration table. */
uint64_t boot_acpi_rsdp(void);

#endif /* _ARCH_X86_64_BOOT_PROTOCOL_H_ */