/* EMBX reader + build_id verifier -- see embxinfo.h. */
#include "embxinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/sha256.h"     /* kernel/crypto (via -Ikernel), also in userspace */

/* EMBX §3.1 header field offsets (the format is byte-exact + little-endian). */
#define O_MAGIC       0x00
#define O_HEADER_SIZE 0x0C
#define O_BIN_TYPE    0x10
#define O_ABI         0x14
#define O_CAP_OFF     0x40
#define O_CAP_COUNT   0x44
#define O_CAP_ESIZE   0x46
#define O_BUILD_ID    0x50
#define O_IMAGE_SIZE  0x70
#define O_HDR_CSUM    0x7C
#define EMBX_HDR_SIZE 128

static const uint8_t MAGIC[8] = { 0x7F,'E','M','B','X',0x0D,0x0A,0x1A };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); free(img); return -1; } while (0)

int embx_read_info(const char *path, struct embx_info *out, char *err, size_t errsz) {
    memset(out, 0, sizeof *out);
    if (errsz) err[0] = 0;
    uint8_t *img = NULL;

    FILE *f = fopen(path, "rb");
    if (!f) FAIL("cannot open %s", path);
    fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);
    if (flen < EMBX_HDR_SIZE) { fclose(f); FAIL("%s too small to be an EMBX", path); }
    img = malloc((size_t)flen);
    if (!img) { fclose(f); FAIL("out of memory"); }
    if (fread(img, 1, (size_t)flen, f) != (size_t)flen) { fclose(f); FAIL("short read on %s", path); }
    fclose(f);

    if (memcmp(img + O_MAGIC, MAGIC, 8) != 0)  FAIL("not an EMBX (bad magic)");
    if (rd32(img + O_HEADER_SIZE) != EMBX_HDR_SIZE) FAIL("header_size != 128");
    if (rd16(img + O_BIN_TYPE) != 1)           FAIL("not an EMBX APP (binary_type != 1)");

    uint64_t image_size = rd64(img + O_IMAGE_SIZE);
    if (image_size != (uint64_t)flen) FAIL("image_size %llu != file size %ld",
                                           (unsigned long long)image_size, flen);

    out->abi = rd32(img + O_ABI);
    memcpy(out->build_id, img + O_BUILD_ID, 32);

    /* Recompute build_id: SHA-256 over the whole image with build_id (0x50, 32B)
     * and header_checksum (0x7C, 4B) zeroed (EMBX §3.4). */
    uint8_t *work = malloc((size_t)flen);
    if (!work) FAIL("out of memory");
    memcpy(work, img, (size_t)flen);
    memset(work + O_BUILD_ID, 0, 32);
    memset(work + O_HDR_CSUM, 0, 4);
    uint8_t calc[32];
    sha256(work, (size_t)flen, calc);
    free(work);
    out->build_id_ok = (memcmp(calc, out->build_id, 32) == 0);

    /* Capability table: cap_count entries of 16 bytes, cap_id at entry+0. */
    uint32_t cap_off = rd32(img + O_CAP_OFF);
    uint16_t cap_count = rd16(img + O_CAP_COUNT);
    uint16_t cap_esize = rd16(img + O_CAP_COUNT + 2);   /* 0x46 */
    (void)cap_esize;
    if (cap_count) {
        if (cap_count > PKG_MAX_CAPS) FAIL("EMBX declares %u caps (> %d)", cap_count, PKG_MAX_CAPS);
        if ((uint64_t)cap_off + (uint64_t)cap_count * 16 > image_size) FAIL("cap table out of bounds");
        int prev = 0;
        for (uint16_t k = 0; k < cap_count; k++) {
            uint32_t id = rd32(img + cap_off + k * 16);
            if ((int)id <= prev) FAIL("cap table not sorted ascending / has a duplicate");
            prev = (int)id;
            out->caps[out->ncaps++] = (int)id;
        }
    }

    free(img);
    return 0;
}
