#ifndef EMBLINK_UEFI_UPDATE_BOOT_H
#define EMBLINK_UEFI_UPDATE_BOOT_H
#include "uefi.h"
#include "update/protocol.h"
struct boot_protocol;
#define UPDATE_BOOT_OPTIONS_MAGIC 0x3154504f50554d45ULL
struct update_boot_options {
    uint64_t magic;
    uint32_t size,mode,slot,flags;
    uint8_t state[UPDATE_RECORD_BYTES];
};
int update_boot_child(EFI_HANDLE,unsigned *mode);
/* Returns only for a legacy disk or embedded recovery fallback. Managed normal
 * boot chains to the selected verified EFI before reserving kernel pages. */
void update_boot_dispatch(EFI_HANDLE,unsigned mode);
void update_boot_fill(struct boot_protocol *);
#endif
