#ifndef EMBLINK_INSTALL_H
#define EMBLINK_INSTALL_H
#include <stdint.h>
#include <stddef.h>

/* The clone engine has no global storage and no access to the block registry.
 * Its caller chooses two whole disks and keeps the source read-only. */
struct install_io {
    void *ctx;
    uint64_t sectors;
    int (*read)(void *, uint64_t, uint32_t, void *);
    int (*write)(void *, uint64_t, uint32_t, const void *);
    int (*flush)(void *);
};
struct install_plan {
    uint8_t mbr[512], header[512], entries[16384];
    uint64_t source_sectors,source_gpt_sectors,target_sectors,copy_end;
    uint64_t esp_start, esp_sectors, root_start, root_sectors;
    uint64_t target_root_sectors,filesystem_blocks,filesystem_free;
    uint32_t filesystem_block_size;
    uint32_t fingerprint;
    uint32_t managed;
    uint8_t update_record[512];
};
int install_prepare(const struct install_io *, const struct install_io *, struct install_plan *);
int install_copy(const struct install_io *, const struct install_io *, struct install_plan *,
                 const uint8_t identities[52], void *buffer, void *verify, size_t buffer_size,
                 void (*progress)(uint64_t, uint64_t));
int install_command(const char *);
#endif
