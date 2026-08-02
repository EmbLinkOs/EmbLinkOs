/* On-OS EMBX producer -- see embxgen.h. A faithful C port of mkembx.py: parse a
 * static ELF64's PT_LOAD segments and repackage them into an EMBX APP with a
 * capability table, matching EMBX_Specification_v2 §3 byte-for-byte (so the
 * build_id equals mkembx's for the same input). */
#include "embxgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto/sha256.h"     /* via -Ikernel */

#define HDR_SIZE 128
#define SEG_SIZE 64
#define CAP_SIZE 16
#define MAX_SEG  16
#define EMBX_SEG_LOAD 1
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define SEG_R 1
#define SEG_W 2
#define SEG_X 4

static const uint8_t MAGIC[8] = { 0x7F,'E','M','B','X',0x0D,0x0A,0x1A };

/* CRC32C (Castagnoli, reflected 0x82F63B78) -- must match the kernel loader's
 * check and mkembx's crc32c.py, or the EMBX is rejected at load. */
static uint32_t crc32c(const uint8_t *data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : (crc >> 1);
    }
    return ~crc;
}

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t r64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v<<8)|p[i]; return v; }
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void w64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

struct seg { uint64_t vaddr, off, filesz, memsz, align, file_offset; uint32_t flags, checksum; };

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); free(elf); free(img); return -1; } while (0)

int embxgen_write(const char *elf_path, const int *caps, int ncaps,
                  const char *out_path, char *err, size_t errsz) {
    if (errsz) err[0] = 0;
    uint8_t *elf = NULL, *img = NULL;

    FILE *f = fopen(elf_path, "rb");
    if (!f) { snprintf(err, errsz, "cannot open %s", elf_path); return -1; }
    fseek(f, 0, SEEK_END); long elen = ftell(f); fseek(f, 0, SEEK_SET);
    if (elen < 64) { fclose(f); snprintf(err, errsz, "%s too small for an ELF", elf_path); return -1; }
    elf = malloc((size_t)elen);
    if (!elf) { fclose(f); snprintf(err, errsz, "oom"); return -1; }
    if (fread(elf, 1, (size_t)elen, f) != (size_t)elen) { fclose(f); FAIL("short read"); }
    fclose(f);

    if (memcmp(elf, "\x7f""ELF", 4) != 0 || elf[4] != 2) FAIL("not a 64-bit ELF");
    uint64_t e_entry = r64(elf + 0x18);
    uint64_t e_phoff = r64(elf + 0x20);
    uint16_t e_phentsz = r16(elf + 0x36);
    uint16_t e_phnum = r16(elf + 0x38);

    struct seg segs[MAX_SEG]; int nseg = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        uint64_t base = e_phoff + (uint64_t)i * e_phentsz;
        if (base + 56 > (uint64_t)elen) break;
        const uint8_t *ph = elf + base;
        if (r32(ph + 0x00) != PT_LOAD) continue;
        if (nseg >= MAX_SEG) FAIL("too many PT_LOAD segments");
        uint32_t pf = r32(ph + 0x04);
        uint32_t fl = 0;
        if (pf & PF_R) fl |= SEG_R;
        if (pf & PF_W) fl |= SEG_W;
        if (pf & PF_X) fl |= SEG_X;
        uint64_t align = r64(ph + 0x30); if (align < 4096) align = 4096;
        segs[nseg].off = r64(ph + 0x08);
        segs[nseg].vaddr = r64(ph + 0x10);
        segs[nseg].filesz = r64(ph + 0x20);
        segs[nseg].memsz = r64(ph + 0x28);
        segs[nseg].align = align;
        segs[nseg].flags = fl;
        nseg++;
    }
    if (nseg == 0) FAIL("no PT_LOAD segments");
    /* sort by vaddr (small n, insertion sort) */
    for (int i = 1; i < nseg; i++) { struct seg t = segs[i]; int j = i - 1;
        while (j >= 0 && segs[j].vaddr > t.vaddr) { segs[j+1] = segs[j]; j--; } segs[j+1] = t; }

    size_t seg_tab = HDR_SIZE;
    size_t cap_tab = seg_tab + (size_t)nseg * SEG_SIZE;
    size_t payload = cap_tab + (size_t)ncaps * CAP_SIZE;

    uint64_t cur = payload;
    for (int i = 0; i < nseg; i++) {
        /* congruent offset: cur + ((vaddr - cur) mod align) */
        uint64_t a = segs[i].align;
        uint64_t rem = ((segs[i].vaddr % a) + a - (cur % a)) % a;
        segs[i].file_offset = cur + rem;
        segs[i].checksum = segs[i].filesz ? crc32c(elf + segs[i].off, segs[i].filesz) : 0;
        cur = segs[i].file_offset + segs[i].filesz;
    }
    uint64_t image_size = cur;

    img = calloc(1, image_size);
    if (!img) FAIL("oom");

    /* segment table */
    for (int i = 0; i < nseg; i++) {
        uint8_t *e = img + seg_tab + (size_t)i * SEG_SIZE;
        w32(e + 0x00, EMBX_SEG_LOAD);
        w32(e + 0x04, segs[i].flags);
        w64(e + 0x08, segs[i].vaddr);
        w64(e + 0x10, segs[i].file_offset);
        w64(e + 0x18, segs[i].filesz);
        w64(e + 0x20, segs[i].memsz);
        w64(e + 0x28, segs[i].align);
        w32(e + 0x30, segs[i].checksum);
        /* 0x34 reserved, 0x38 paddr both zero */
    }
    /* capability table */
    for (int i = 0; i < ncaps; i++) w32(img + cap_tab + (size_t)i * CAP_SIZE, (uint32_t)caps[i]);
    /* payloads */
    for (int i = 0; i < nseg; i++)
        if (segs[i].filesz) memcpy(img + segs[i].file_offset, elf + segs[i].off, segs[i].filesz);

    /* header (build_id + header_checksum stay zero for the §3.4 ordering) */
    memcpy(img + 0x00, MAGIC, 8);
    w16(img + 0x08, 1); w16(img + 0x0A, 0);            /* version */
    w32(img + 0x0C, HDR_SIZE);
    w16(img + 0x10, 1); w16(img + 0x12, 1);            /* type APP, machine x64 */
    w32(img + 0x14, 1);                                /* abi */
    w64(img + 0x30, e_entry);
    w32(img + 0x38, (uint32_t)seg_tab); w16(img + 0x3C, (uint16_t)nseg); w16(img + 0x3E, SEG_SIZE);
    w32(img + 0x40, ncaps ? (uint32_t)cap_tab : 0); w16(img + 0x44, (uint16_t)ncaps); w16(img + 0x46, CAP_SIZE);
    w64(img + 0x70, image_size);

    uint8_t bid[32];
    sha256(img, image_size, bid);                      /* build_id fields still zero */
    memcpy(img + 0x50, bid, 32);
    w32(img + 0x7C, crc32c(img, 0x7C));

    FILE *o = fopen(out_path, "wb");
    if (!o) FAIL("cannot write %s", out_path);
    size_t w = fwrite(img, 1, image_size, o);
    fclose(o);
    if (w != image_size) FAIL("short write to %s", out_path);

    free(elf); free(img);
    return 0;
}
