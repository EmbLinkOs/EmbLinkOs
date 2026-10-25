/* intel_fixtures.h -- shared test fixtures: an EDID and a VBT shaped like a
 * Comet Lake laptop's. Included by the Intel display host tests. */
#ifndef EMBLINK_TEST_INTEL_FIXTURES_H
#define EMBLINK_TEST_INTEL_FIXTURES_H
#include <stdint.h>
#include <string.h>

static inline void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static inline void make_edid(uint8_t *e, const char *mfg, unsigned product, unsigned hact, unsigned vact,
                      unsigned clock_10khz, const char *name)
{
    memset(e, 0, 256);
    static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    memcpy(e, header, 8);
    unsigned m = (unsigned)(mfg[0] - 'A' + 1) << 10 | (unsigned)(mfg[1] - 'A' + 1) << 5 | (unsigned)(mfg[2] - 'A' + 1);
    e[8] = (uint8_t)(m >> 8); e[9] = (uint8_t)m;
    put16(e + 10, product);
    e[18] = 1; e[19] = 4;
    e[20] = 0x80 | (2u << 4) | 5;      /* digital, 8 bpc, DisplayPort */
    uint8_t *d = e + 54;                                  /* preferred detailed timing */
    unsigned hbl = 160, vbl = 30;
    put16(d, clock_10khz);
    d[2] = (uint8_t)hact; d[3] = (uint8_t)hbl; d[4] = (uint8_t)((hact >> 8) << 4 | (hbl >> 8));
    d[5] = (uint8_t)vact; d[6] = (uint8_t)vbl; d[7] = (uint8_t)((vact >> 8) << 4 | (vbl >> 8));
    d[8] = 48; d[9] = 32; d[10] = 3 << 4 | 5; d[11] = 0;
    d[17] = 0x1a;
    uint8_t *n = e + 72;                                  /* monitor name descriptor */
    n[3] = 0xfc;
    unsigned i = 0;
    for (; name[i] && i < 13; i++) n[5 + i] = (uint8_t)name[i];
    if (i < 13) n[5 + i++] = '\n';
    for (; i < 13; i++) n[5 + i] = ' ';
    unsigned sum = 0;
    for (unsigned k = 0; k < 127; k++) sum += e[k];
    e[127] = (uint8_t)(256 - (sum & 0xff));
}

/* A VBT shaped like a Comet Lake laptop's: eDP on DDI A, HDMI on DDI B,
 * DP-over-USB-C on DDI C, panel type 2 with power-sequence delays, PCH PWM
 * backlight, and the panel's DTD behind the LFP pointer table. */
static inline uint32_t make_vbt(uint8_t *v)
{
    memset(v, 0, 0x2000);
    memcpy(v, "$VBT COMETLAKE      ", 20);
    put16(v + 20, 100); put16(v + 22, 48);
    put32(v + 32, 48);                                   /* bdb_offset */
    uint8_t *bdb = v + 48;
    memcpy(bdb, "BIOS_DATA_BLOCK ", 16);
    put16(bdb + 16, 228); put16(bdb + 18, 22);
    uint8_t *p = bdb + 22;

    /* block 2: general definitions, three 39-byte child devices (v216-255) */
    p[0] = 2; put16(p + 1, 5 + 3 * 39);
    uint8_t *g = p + 3;
    g[4] = 39;
    uint8_t *c = g + 5;
    put16(c + 0, 0x0008); put16(c + 2, 0x78c6); c[16] = 10; c[24] = 0x2; c[25] = 0x40; c[38] = 0;   /* eDP, DDI A */
    c += 39;
    put16(c + 0, 0x0004); put16(c + 2, 0x60d2); c[7] = 8; c[16] = 1; c[19] = 0x1; c[24] = 0x1;      /* HDMI, DDI B, level 8 */
    c += 39;
    put16(c + 0, 0x0002); put16(c + 2, 0x68c6); c[16] = 8; c[19] = 0x2; c[24] = 0x2; c[25] = 0x20; c[33] = 1; /* DP, DDI C, Type-C */
    p += 3 + 5 + 3 * 39;

    /* block 27: eDP, power sequences for 16 panel types */
    p[0] = 27; put16(p + 1, 16 * 10 + 4);
    uint8_t *ps = p + 3 + 2 * 10;                        /* panel type 2 */
    put16(ps, 2000); put16(ps + 2, 500); put16(ps + 4, 500); put16(ps + 6, 500); put16(ps + 8, 5000);
    p += 3 + 16 * 10 + 4;

    /* block 40: LFP options -- panel type 2 */
    p[0] = 40; put16(p + 1, 4); p[3] = 2;
    p += 3 + 4;

    /* block 41: LFP data pointers -> block 42's DTD for panel 2 */
    uint8_t *ptrs = p;
    p[0] = 41; put16(p + 1, 1 + 16 * 9);
    p += 3 + 1 + 16 * 9;
    uint8_t *lfp = p;
    p[0] = 42; put16(p + 1, 18 * 16);
    uint8_t *dtd = lfp + 3 + 2 * 18;
    put16(dtd, 14250); dtd[2] = 0x80; dtd[3] = 0xa0; dtd[4] = 0x70; dtd[5] = 0x38; dtd[6] = 0x1e; dtd[7] = 0x40;
    dtd[8] = 48; dtd[9] = 32; dtd[10] = 0x35; dtd[17] = 0x80 | 0x40;   /* progressive, H+ V- (VBT flag layout) */
    put16(ptrs + 3 + 1 + 2 * 9 + 3, (unsigned)(dtd - bdb)); ptrs[3 + 1 + 2 * 9 + 5] = 18;
    p += 3 + 18 * 16;

    /* block 43: backlight, 6-byte entries; panel 2 = PCH PWM at 200 Hz */
    p[0] = 43; put16(p + 1, 1 + 16 * 6);
    p[3] = 6;
    uint8_t *bl = p + 4 + 2 * 6;
    bl[0] = 2; put16(bl + 1, 200); bl[3] = 10;
    p += 3 + 1 + 16 * 6;

    uint32_t bdb_size = (uint32_t)(p - bdb);
    put16(bdb + 20, bdb_size);
    uint32_t total = 48 + bdb_size;
    put16(v + 24, total);
    unsigned sum = 0;
    for (uint32_t i = 0; i < total; i++) sum += v[i];
    v[26] = (uint8_t)(256 - (sum & 0xff));
    return total;
}

#endif
