/* Minimal ZIP reader (see unzip.h). Uses the central directory as the source of
 * truth (name, sizes, method, local-header offset), then finds the entry data via
 * the local header. STORED (0) and DEFLATE (8) only. */
#include "unzip.h"
#include "inflate.h"
#include <string.h>
#include <stdlib.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }

#define SIG_EOCD  0x06054b50u
#define SIG_CD    0x02014b50u
#define SIG_LOCAL 0x04034b50u

int unzip_iter(const uint8_t *zip, size_t zip_len, unzip_cb cb, void *ctx) {
    if (zip_len < 22) return -1;

    /* Find the End Of Central Directory record: scan backward for its signature
     * (the trailing comment is usually empty, so it's near the end). */
    size_t eocd = 0; int found = 0;
    size_t start = zip_len >= 22 + 65535 ? zip_len - (22 + 65535) : 0;
    for (size_t i = zip_len - 22; ; i--) {
        if (rd32(zip + i) == SIG_EOCD) { eocd = i; found = 1; break; }
        if (i == start) break;
    }
    if (!found) return -1;

    uint16_t n_entries = rd16(zip + eocd + 10);
    uint32_t cd_size   = rd32(zip + eocd + 12);
    uint32_t cd_off    = rd32(zip + eocd + 16);
    if ((size_t)cd_off + cd_size > zip_len) return -1;

    const uint8_t *p = zip + cd_off, *cd_end = zip + cd_off + cd_size;
    for (int e = 0; e < n_entries; e++) {
        if (p + 46 > cd_end || rd32(p) != SIG_CD) return -1;
        uint16_t method   = rd16(p + 10);
        uint32_t comp_sz  = rd32(p + 20);
        uint32_t uncomp_sz= rd32(p + 24);
        uint16_t name_len = rd16(p + 28);
        uint16_t extra_len= rd16(p + 30);
        uint16_t cmt_len  = rd16(p + 32);
        uint32_t lho      = rd32(p + 42);       /* local header offset */
        const char *name  = (const char *)(p + 46);
        if (p + 46 + name_len > cd_end) return -1;

        /* Locate the entry data via its local header (its name/extra lengths can
         * differ from the central directory's). */
        if ((size_t)lho + 30 > zip_len || rd32(zip + lho) != SIG_LOCAL) return -1;
        uint16_t lname = rd16(zip + lho + 26);
        uint16_t lextra= rd16(zip + lho + 28);
        const uint8_t *data = zip + lho + 30 + lname + lextra;
        if (data + comp_sz > zip + zip_len) return -1;

        /* Skip directory entries (name ends in '/', zero content). */
        int is_dir = name_len > 0 && name[name_len - 1] == '/';
        if (!is_dir) {
            const uint8_t *out; uint8_t *dyn = NULL;
            if (method == 0) {                  /* STORED */
                if (comp_sz != uncomp_sz) return -1;
                out = data;
            } else if (method == 8) {           /* DEFLATE */
                dyn = malloc(uncomp_sz ? uncomp_sz : 1);
                if (!dyn) return -2;
                size_t got = 0;
                if (inflate_raw(data, comp_sz, dyn, uncomp_sz, &got) != 0 || got != uncomp_sz) {
                    free(dyn); return -3;
                }
                out = dyn;
            } else {
                return -4;                      /* unsupported method */
            }
            int r = cb(ctx, name, name_len, out, uncomp_sz);
            free(dyn);
            if (r) return r;
        }

        p += 46 + name_len + extra_len + cmt_len;
    }
    return 0;
}
