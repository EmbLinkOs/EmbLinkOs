#ifndef EMBLINK_UPDATE_PROTOCOL_H
#define EMBLINK_UPDATE_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

/* Fixed little-endian boot journal, shared by firmware, kernel and publisher.
 * Two independently flushed records retain a usable predecessor after a torn
 * sector write. User data is a different partition and never a rollback slot. */
#define UPDATE_RECORD_BYTES 512
#define UPDATE_VERSION_BYTES 32
#define UPDATE_NO_SLOT 255
#define UPDATE_REVERTED 1u
#define UPDATE_WRITING 2u
#define UPDATE_BOOT_FAILED 1u
#define UPDATE_IMAGE_FAILED 2u
#define UPDATE_WRITE_FAILED 3u
#define UPDATE_CONTROL_BYTES 4096u
#define UPDATE_CONTROL0_OFFSET (16u<<10)
#define UPDATE_CONTROL1_OFFSET (32u<<10)
#define UPDATE_FACTORY_OFFSET (48u<<10)
#define UPDATE_MAX_EFI_BYTES (16ULL<<20)
#define UPDATE_EFI_CAPACITY UPDATE_MAX_EFI_BYTES
#define UPDATE_MAX_ROOT_BYTES (8ULL<<30)

struct update_slot {
    uint8_t root_guid[16];
    uint64_t release;
    uint64_t root_bytes;
    uint64_t efi_bytes;
    char version[UPDATE_VERSION_BYTES];
    uint8_t efi_sha256[32];
    uint8_t root_sha256[32];
};
struct update_state {
    uint64_t revision;
    uint32_t flags;
    uint8_t active,pending,previous,attempts;
    uint8_t data_guid[16],esp_guid[16];
    struct update_slot slot[2];
    uint32_t last_error;
};
enum update_result {
    UPDATE_OK=0, UPDATE_INVALID=-1, UPDATE_BUSY=-2,
    UPDATE_OLDER=-3, UPDATE_EXHAUSTED=-4, UPDATE_NOT_TRIAL=-5
};
uint32_t update_crc32(const void *,size_t);
int update_state_valid(const struct update_state *);
int update_state_encode(const struct update_state *,uint8_t out[UPDATE_RECORD_BYTES]);
int update_state_decode(const uint8_t *,size_t,struct update_state *);
/* Returns selected record 0/1, or UPDATE_INVALID if neither record is valid.
 * Equal revisions with different content are ambiguous and rejected. */
int update_state_choose(const uint8_t *,const uint8_t *,struct update_state *);
int update_state_stage(struct update_state *,unsigned slot,const struct update_slot *);
int update_state_begin_write(struct update_state *,unsigned inactive);
/* Before booting a trial, persist the mutated state. On another boot without
 * confirmation, pending is cleared and the confirmed slot is selected. */
int update_state_boot(struct update_state *,unsigned *slot,int *changed);
int update_state_confirm(struct update_state *,unsigned running);
int update_state_rollback(struct update_state *);
int update_state_reject(struct update_state *,uint32_t reason);
#endif
