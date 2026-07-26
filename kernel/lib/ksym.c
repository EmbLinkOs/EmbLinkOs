/* Kernel panic symbolizer — the in-kernel .embdbg reader (EMBDBG_Specification
 * .md §7). The format is designed to be read exactly like this: a header, a
 * section table, and fixed-size arrays sorted for binary search — no state
 * machine, no allocation, no decode path that never runs. We read only what a
 * panic needs: FUNCS (addr -> function) and LINE (addr -> file:line), through
 * STRTAB. Little-endian, byte-wise (the image may be unaligned). */

#include "lib/ksym.h"
#include "include/kstring.h"
#include "include/kprintf.h"   /* snprintf */

static const uint8_t MAGIC[8] = { 0x7F,0x45,0x4D,0x44,0x42,0x47,0x0A,0x1A };

/* section kinds (debug_abi.h / the writer) */
#define K_STRTAB 1
#define K_FILES  2
#define K_LINE   3
#define K_FUNCS  4

static struct {
    int loaded;
    const char    *strtab;
    const uint8_t *funcs;  uint32_t nfuncs;  /* 32-byte rows, sorted by low_pc */
    const uint8_t *lines;  uint32_t nlines;  /* 16-byte rows, sorted by addr   */
    const uint8_t *files;  uint32_t nfiles;  /* 40-byte rows                   */
} g;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

int ksym_load(const void *data, uint32_t len) {
    const uint8_t *b = (const uint8_t *)data;
    if (len < 64 || memcmp(b, MAGIC, 8) != 0) return -1;
    uint16_t nsec = rd16(b + 0x30);
    uint32_t tab  = rd32(b + 0x34);
    if ((uint64_t)tab + (uint64_t)nsec * 24 > len) return -1;
    for (uint16_t i = 0; i < nsec; i++) {
        const uint8_t *e = b + tab + (uint32_t)i * 24;
        uint16_t kind = rd16(e);
        uint32_t count = rd32(e + 8), off = rd32(e + 12), size = rd32(e + 16);
        if ((uint64_t)off + size > len) continue;
        const uint8_t *body = b + off;
        if (kind == K_STRTAB) g.strtab = (const char *)body;
        else if (kind == K_FILES) { g.files = body; g.nfiles = count; }
        else if (kind == K_LINE)  { g.lines = body; g.nlines = count; }
        else if (kind == K_FUNCS) { g.funcs = body; g.nfuncs = count; }
    }
    if (!g.funcs || !g.strtab) return -1;
    g.loaded = 1;
    return 0;
}

int ksym_ready(void) { return g.loaded; }

/* The function whose [low_pc, high_pc) contains addr, or NULL. */
static const char *find_func(uint64_t addr, uint64_t *lo_out) {
    if (!g.loaded || !g.funcs || g.nfuncs == 0) return 0;
    int lo = 0, hi = (int)g.nfuncs - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rd64(g.funcs + (uint32_t)mid * 32) <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return 0;
    const uint8_t *e = g.funcs + (uint32_t)best * 32;
    uint64_t l = rd64(e), h = rd64(e + 8);
    if (addr < l || addr >= h) return 0;
    if (lo_out) *lo_out = l;
    return g.strtab + rd32(e + 16);
}

/* The source line for addr (greatest LINE row with addr <= target), 0 = none. */
static int find_line(uint64_t addr, const char **file_out) {
    if (!g.loaded || !g.lines || g.nlines == 0) return 0;
    int lo = 0, hi = (int)g.nlines - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rd64(g.lines + (uint32_t)mid * 16) <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return 0;
    const uint8_t *e = g.lines + (uint32_t)best * 16;
    int line = (int)rd32(e + 8);
    if (line == 0) return 0;                 /* end-of-sequence marker */
    uint16_t fi = rd16(e + 12);
    if (file_out) {
        if (g.files && fi < g.nfiles) *file_out = g.strtab + rd32(g.files + (uint32_t)fi * 40);
        else *file_out = 0;
    }
    return line;
}

void ksym_symbolize(uint64_t addr, char *out, size_t n) {
    uint64_t lo = 0;
    const char *fn = find_func(addr, &lo);
    const char *file = 0;
    int line = find_line(addr, &file);
    if (fn && file && line) snprintf(out, n, "%s+0x%lx (%s:%d)", fn, (unsigned long)(addr - lo), file, line);
    else if (fn)            snprintf(out, n, "%s+0x%lx", fn, (unsigned long)(addr - lo));
    else                    snprintf(out, n, "0x%lx ?", (unsigned long)addr);
}
