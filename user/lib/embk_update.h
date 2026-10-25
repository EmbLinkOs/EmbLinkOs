#ifndef EMBK_UPDATE_H
#define EMBK_UPDATE_H
#include <stdint.h>
/* Fixed-width ABI; no pointers inside records. */
enum { EMBK_UPDATE_STATUS=0, EMBK_UPDATE_CHECK, EMBK_UPDATE_INSTALL, EMBK_UPDATE_ROLLBACK,
    EMBK_UPDATE_WORKER, EMBK_UPDATE_JOB, EMBK_UPDATE_REPORT, EMBK_UPDATE_BEGIN,
    EMBK_UPDATE_WRITE_ROOT, EMBK_UPDATE_WRITE_EFI, EMBK_UPDATE_COMMIT, EMBK_UPDATE_ABORT, EMBK_UPDATE_APPLICATIONS };
enum { EMBK_UPDATE_UNAVAILABLE=0, EMBK_UPDATE_IDLE, EMBK_UPDATE_CHECKING,
    EMBK_UPDATE_AVAILABLE, EMBK_UPDATE_DOWNLOADING, EMBK_UPDATE_VERIFYING,
    EMBK_UPDATE_WRITING, EMBK_UPDATE_RESTART, EMBK_UPDATE_ERROR };
#define EMBK_UPDATE_MANAGED 1u
#define EMBK_UPDATE_ADMIN 2u
#define EMBK_UPDATE_PREVIOUS 4u
#define EMBK_UPDATE_TRIAL 8u
#define EMBK_UPDATE_REVERTED 16u
#define EMBK_UPDATE_APPS_CHANNEL 32u
struct embk_update_info {
    uint32_t size,phase,flags,job;
    int32_t error;uint32_t progress;
    uint64_t release,candidate_release,done,total;
    char current[32],candidate[32],message[160],notes[256];
};
struct embk_update_image {
    uint64_t release,root_bytes,efi_bytes;
    char version[32];uint8_t root_sha256[32],efi_sha256[32];
};
#define EMBK_UPDATE_CHUNK 32768u
#endif
