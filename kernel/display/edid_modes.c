/* edid_modes.c -- every mode a monitor's EDID offers, as timings a driver can
 * program: the detailed timings of the base block and of CTA-861 extensions,
 * the CTA video codes, the established timings and the standard timings.
 *
 * Detailed timings carry their own numbers. The other three name a mode and
 * leave its timing to a standard, so those are looked up in the tables below:
 * VESA DMT 1.13 and CTA-861-G table 1. A named mode this file has no timing
 * for is skipped rather than guessed. */
#include "display/edid_modes.h"

static uint16_t u16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* VESA DMT and CTA-861 timings this file can name. Flags: H/V sync polarity. */
#define HP DISPLAY_MODE_HSYNC_POSITIVE
#define VP DISPLAY_MODE_VSYNC_POSITIVE
static const struct display_mode known[] = {
    /* clock   hdisp hss  hse  htot  vdisp vss  vse  vtot  flags   vic */
    {  25175,  640,  656,  752,  800,  480,  490,  492,  525, 0,       1 },   /* DMT 640x480@60, CTA 1 */
    {  27000,  720,  736,  798,  858,  480,  489,  495,  525, 0,       2 },   /* CTA 2/3 480p */
    {  27000,  720,  732,  796,  864,  576,  581,  586,  625, 0,      17 },   /* CTA 17/18 576p */
    {  40000,  800,  840,  968, 1056,  600,  601,  605,  628, HP | VP, 0 },   /* DMT 800x600@60 */
    {  65000, 1024, 1048, 1184, 1344,  768,  771,  777,  806, 0,       0 },   /* DMT 1024x768@60 */
    {  74250, 1280, 1390, 1430, 1650,  720,  725,  730,  750, HP | VP, 4 },   /* CTA 4 720p60 */
    {  74250, 1280, 1720, 1760, 1980,  720,  725,  730,  750, HP | VP, 19 },  /* CTA 19 720p50 */
    {  83500, 1280, 1352, 1480, 1680,  800,  803,  809,  831, VP,      0 },   /* DMT 1280x800@60 */
    { 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, HP | VP, 0 },   /* DMT 1280x1024@60 */
    {  85500, 1366, 1436, 1579, 1792,  768,  771,  774,  798, HP | VP, 0 },   /* DMT 1366x768@60 */
    { 106500, 1440, 1520, 1672, 1904,  900,  903,  909,  934, VP,      0 },   /* DMT 1440x900@60 */
    { 108000, 1600, 1624, 1704, 1800,  900,  901,  904, 1000, HP | VP, 0 },   /* DMT 1600x900@60 RB */
    { 146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, VP,      0 },   /* DMT 1680x1050@60 */
    { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, HP | VP, 16 },  /* CTA 16 1080p60 */
    { 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, HP | VP, 31 },  /* CTA 31 1080p50 */
    {  74250, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, HP | VP, 32 },  /* CTA 32 1080p24 */
    {  74250, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, HP | VP, 33 },  /* CTA 33 1080p25 */
    {  74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, HP | VP, 34 },  /* CTA 34 1080p30 */
    { 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, HP,      0 },   /* DMT 1920x1200@60 RB */
    { 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, HP,      0 },   /* CVT-RB 2560x1440@60 */
    { 297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, HP | VP, 93 },  /* CTA 93 2160p24 */
    { 297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, HP | VP, 94 },  /* CTA 94 2160p25 */
    { 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, HP | VP, 95 },  /* CTA 95 2160p30 */
    { 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, HP | VP, 97 },  /* CTA 97 2160p60 */
};
#undef HP
#undef VP
#define KNOWN (sizeof known / sizeof known[0])

static const struct display_mode *by_vic(unsigned vic)
{
    if (vic == 3) vic = 2;                     /* 16:9 twin of 480p */
    if (vic == 18) vic = 17;                   /* 16:9 twin of 576p */
    for (unsigned i = 0; i < KNOWN; i++)
        if (known[i].vic == vic) return &known[i];
    return 0;
}

static const struct display_mode *by_size(unsigned w, unsigned h, unsigned hz)
{
    for (unsigned i = 0; i < KNOWN; i++) {
        uint32_t r = display_mode_refresh_mhz(&known[i]);
        if (known[i].hdisplay == w && known[i].vdisplay == h && r + 1000 > hz * 1000 && r < hz * 1000 + 1000)
            return &known[i];
    }
    return 0;
}

struct list { struct display_mode *out; unsigned max, n; };

static void add(struct list *l, const struct display_mode *m, uint8_t extra_flags)
{
    if (!display_mode_sane(m)) return;
    uint32_t r = display_mode_refresh_mhz(m);
    for (unsigned i = 0; i < l->n; i++) {
        const struct display_mode *o = &l->out[i];
        uint32_t ro = display_mode_refresh_mhz(o);
        if (o->hdisplay == m->hdisplay && o->vdisplay == m->vdisplay &&
            (ro > r ? ro - r : r - ro) < 500) {
            l->out[i].flags |= extra_flags & DISPLAY_MODE_PREFERRED;
            return;                            /* the same mode, named twice */
        }
    }
    if (l->n < l->max) {
        l->out[l->n] = *m;
        l->out[l->n].flags |= extra_flags;
        l->n++;
    }
}

int edid_parse_dtd(const uint8_t *d, struct display_mode *m)
{
    uint32_t clock = u16le(d) * 10u;
    if (!clock) return -1;
    unsigned hact = d[2] | (d[4] >> 4) << 8, hbl = d[3] | (d[4] & 0xfu) << 8;
    unsigned vact = d[5] | (d[7] >> 4) << 8, vbl = d[6] | (d[7] & 0xfu) << 8;
    unsigned hso = d[8] | ((d[11] >> 6) & 3u) << 8, hsw = d[9] | ((d[11] >> 4) & 3u) << 8;
    unsigned vso = (d[10] >> 4) | ((d[11] >> 2) & 3u) << 4, vsw = (d[10] & 0xfu) | (d[11] & 3u) << 4;
    m->clock_khz = clock;
    m->hdisplay = (uint16_t)hact;
    m->hsync_start = (uint16_t)(hact + hso);
    m->hsync_end = (uint16_t)(hact + hso + hsw);
    m->htotal = (uint16_t)(hact + hbl);
    m->vdisplay = (uint16_t)vact;
    m->vsync_start = (uint16_t)(vact + vso);
    m->vsync_end = (uint16_t)(vact + vso + vsw);
    m->vtotal = (uint16_t)(vact + vbl);
    m->flags = 0;
    m->vic = 0;
    if (d[17] & 0x80) m->flags |= DISPLAY_MODE_INTERLACED;
    if ((d[17] & 0x18) == 0x18) {              /* digital separate sync: polarities given */
        if (d[17] & 0x04) m->flags |= DISPLAY_MODE_VSYNC_POSITIVE;
        if (d[17] & 0x02) m->flags |= DISPLAY_MODE_HSYNC_POSITIVE;
    }
    return 0;
}

static int block_ok(const uint8_t *b)
{
    unsigned sum = 0;
    for (unsigned i = 0; i < 128; i++) sum += b[i];
    return (sum & 0xffu) == 0;
}

static void cta_block(struct list *l, const uint8_t *b)
{
    unsigned dtd_at = b[2];
    if (b[1] >= 3 && dtd_at >= 4 && dtd_at <= 127) {
        for (unsigned at = 4; at < dtd_at; ) {
            unsigned tag = b[at] >> 5, len = b[at] & 0x1fu;
            if (at + 1 + len > dtd_at) break;
            if (tag == 2) {                     /* video data block */
                for (unsigned i = 0; i < len; i++) {
                    unsigned svd = b[at + 1 + i], vic = (svd >= 129 && svd <= 192) ? svd & 0x7fu : svd;
                    const struct display_mode *k = by_vic(vic);
                    if (k) {
                        struct display_mode m = *k;
                        m.vic = (uint8_t)vic;
                        add(l, &m, (svd >= 129 && svd <= 192) ? DISPLAY_MODE_PREFERRED : 0);
                    }
                }
            }
            at += 1 + len;
        }
    }
    for (unsigned at = dtd_at; at >= 4 && at + 18 <= 127; at += 18) {
        struct display_mode m;
        if (edid_parse_dtd(b + at, &m)) break;
        add(l, &m, 0);
    }
}

int edid_modes(const uint8_t *edid, unsigned len, struct display_mode *out, unsigned max)
{
    static const uint8_t magic[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (!edid || len < 128 || !out || !max) return -1;
    for (unsigned i = 0; i < 8; i++) if (edid[i] != magic[i]) return -1;
    if (!block_ok(edid) || edid[18] != 1) return -1;

    struct list l = { out, max, 0 };
    /* The first detailed timing is the preferred mode in EDID 1.3 when the
     * feature byte says so, and always in 1.4. */
    int first_preferred = edid[19] >= 4 || (edid[24] & 0x02);
    for (unsigned i = 0; i < 4; i++) {
        struct display_mode m;
        if (!edid_parse_dtd(edid + 54 + i * 18, &m))
            add(&l, &m, (i == 0 && first_preferred) ? DISPLAY_MODE_PREFERRED : 0);
    }
    for (unsigned e = 1; e <= edid[126] && (e + 1) * 128u <= len; e++) {
        const uint8_t *b = edid + e * 128;
        if (b[0] == 0x02 && block_ok(b)) cta_block(&l, b);
    }
    static const struct { uint8_t byte, bit; uint16_t w, h; uint8_t hz; } established[] = {
        { 35, 5, 640, 480, 60 }, { 35, 0, 800, 600, 60 }, { 36, 3, 1024, 768, 60 },
    };
    for (unsigned i = 0; i < sizeof established / sizeof established[0]; i++)
        if (edid[established[i].byte] & (1u << established[i].bit)) {
            const struct display_mode *k = by_size(established[i].w, established[i].h, established[i].hz);
            if (k) add(&l, k, 0);
        }
    for (unsigned i = 0; i < 8; i++) {
        uint8_t a = edid[38 + i * 2], b = edid[39 + i * 2];
        if (a <= 1 && (b == 0 || b == 1)) continue;     /* unused slot */
        unsigned w = (a + 31u) * 8u, hz = (b & 0x3fu) + 60u, h;
        switch (b >> 6) {
        case 0:  h = edid[19] >= 3 ? w * 10 / 16 : w; break;   /* 16:10 (1:1 before 1.3) */
        case 1:  h = w * 3 / 4; break;
        case 2:  h = w * 4 / 5; break;
        default: h = w * 9 / 16; break;
        }
        if (w == 1360 && h == 765) { w = 1366; h = 768; }       /* how 1366x768 is spelled */
        const struct display_mode *k = by_size(w, h, hz);
        if (k) add(&l, k, 0);
    }
    return (int)l.n;
}

const struct display_mode *edid_known_mode(unsigned width, unsigned height, unsigned hz)
{
    return by_size(width, height, hz);
}
