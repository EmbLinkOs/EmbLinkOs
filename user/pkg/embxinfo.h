#ifndef EMBK_PKG_EMBXINFO_H
#define EMBK_PKG_EMBXINFO_H
/* Read + verify an EMBX executable enough to check a package manifest against it:
 * its abi, its content identity (build_id, recomputed per EMBX §3.4), and its
 * declared capability table. The kernel loader validates magic + CRC32Cs but
 * NOT build_id -- that content-identity check is `pkg`'s job. */
#include <stddef.h>
#include <stdint.h>
#include "manifest.h"          /* PKG_MAX_CAPS */

struct embx_info {
    uint32_t abi;
    uint8_t  build_id[32];         /* as stored in the header (0x50) */
    int      build_id_ok;          /* 1 if the recomputed hash matches */
    int      caps[PKG_MAX_CAPS];   /* cap_ids from the table (0x40), ascending */
    int      ncaps;
};

/* Parse `path` as an EMBX APP: check magic/type/size, recompute build_id
 * (SHA-256 over the whole image with build_id + header_checksum zeroed), and
 * extract abi + the capability table. 0 on success (see out->build_id_ok); -1
 * with a reason in err[0..errsz) on a malformed file. */
int embx_read_info(const char *path, struct embx_info *out, char *err, size_t errsz);

#endif /* EMBK_PKG_EMBXINFO_H */
