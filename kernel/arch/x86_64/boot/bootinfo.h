#ifndef _ARCH_X86_64_BOOTINFO_H_
#define _ARCH_X86_64_BOOTINFO_H_

#include <stdint.h>
#include "include/types.h"

/* Boot-device facts stage2 hands the kernel through a fixed low-memory record.
 *
 * stage2 fills the struct at BOOTINFO_PHYS just before switching to protected
 * mode, while the MBR it was booted from is still resident at 0x7C00 -- so the
 * MBR disk signature (offset 0x1B8) and the BIOS boot drive (DL) survive the
 * handoff into the kernel. Same low-memory-convention trick the e820 map uses
 * (0x7000); 0x6F00 sits in the free gap between the VBE block (~0x5000) and the
 * e820 buffer (0x7000), below the AP trampoline (0x8000).
 *
 * Read ONCE, early in kernel_main (bootinfo_capture), before the PMM can hand
 * that physical frame out to an allocation. */

#define BOOTINFO_PHYS   0x6F00UL
#define BOOTINFO_MAGIC  0x4B534442UL   /* 'BDSK', little-endian -- validity mark */

/* Capture the boot-info record into a kernel-owned copy. Call once, early. */
void bootinfo_capture(void);

/* The MBR disk signature (LBA 0, offset 0x1B8) of the disk we booted from.
 * Returns true and writes *out ONLY when the record was valid AND the signature
 * is nonzero. A zero signature -- e.g. the unpartitioned two-image IDE boot disk,
 * whose sector 0 is bare stage1 with no disk id -- reads as "unknown", and every
 * caller then falls back to its default (earliest-enumerated) choice. */
bool bootinfo_boot_disk_sig(uint32_t *out);

/* The BIOS drive number (DL) we booted from, or 0xFF if unknown. Informational. */
uint8_t bootinfo_boot_drive(void);

#endif /* _ARCH_X86_64_BOOTINFO_H_ */
