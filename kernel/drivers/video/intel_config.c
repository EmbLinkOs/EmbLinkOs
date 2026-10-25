/* intel_config.c -- the arithmetic of a Gen9 modeset. See intel_config.h. */
#include "drivers/video/intel_config.h"

#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

/* ---- DisplayPort ---------------------------------------------------------- */

int intel_dp_caps_parse(const uint8_t d[16], const uint8_t *edp_rates, struct intel_dp_caps *c)
{
    for (unsigned i = 0; i < sizeof *c; i++) ((uint8_t *)c)[i] = 0;
    if (!d || d[0] == 0) return -1;
    c->revision = d[0];
    c->max_lanes = d[2] & 0x1f;
    if (c->max_lanes != 1 && c->max_lanes != 2 && c->max_lanes != 4) c->max_lanes = c->max_lanes >= 4 ? 4 : 1;
    c->tps3 = (d[2] >> 6) & 1;
    c->enhanced_framing = (d[2] >> 7) & 1;
    c->downspread = d[3] & 1;
    c->training_interval = d[0xe] & 0x7f;
    if (edp_rates) {
        /* eDP 1.4: up to eight rates, 200 kHz units of bit rate (so x20 for
         * the symbol clock), ascending, ended by a zero. */
        uint32_t last = 0;
        for (unsigned i = 0; i < 8; i++) {
            uint32_t v = (uint32_t)edp_rates[2 * i] | (uint32_t)edp_rates[2 * i + 1] << 8;
            if (!v) break;
            uint32_t rate = v * 20u;
            if (rate <= last) break;          /* not ascending: the table cannot be trusted */
            c->rates[c->n_rates] = rate;
            c->rate_index[c->n_rates] = (uint8_t)i;
            c->n_rates++;
            last = rate;
        }
        if (c->n_rates) { c->rate_table = 1; return 0; }
    }
    static const uint32_t std[] = { 162000, 270000, 540000, 810000 };
    uint32_t max = (uint32_t)d[1] * 27000u;
    for (unsigned i = 0; i < 4; i++)
        if (std[i] <= max) c->rates[c->n_rates++] = std[i];
    return c->n_rates ? 0 : -1;
}

static int usable(const struct intel_dp_caps *c, const struct intel_dp_source *s, unsigned i)
{
    uint32_t r = c->rates[i];
    return r <= s->max_rate && (!s->rate_ok || s->rate_ok(r, s->ctx));
}

static uint64_t need_kbytes(const struct display_mode *m, unsigned bpp)
{
    return DIV_ROUND_UP((uint64_t)m->clock_khz * bpp, 8);
}

static void fill(const struct intel_dp_caps *c, unsigned i, unsigned lanes, unsigned bpp,
                 struct intel_dp_link *l)
{
    l->rate = c->rates[i];
    l->lanes = (uint8_t)lanes;
    l->bpp = (uint8_t)bpp;
    if (c->rate_table) { l->bw_code = 0; l->rate_select = c->rate_index[i]; }
    else { l->bw_code = (uint8_t)(c->rates[i] / 27000u); l->rate_select = 0xff; }
}

int intel_dp_pick_link(const struct intel_dp_caps *c, const struct intel_dp_source *s,
                       const struct display_mode *m, int edp, unsigned panel_bpp,
                       int vbt_rate_khz, unsigned vbt_lanes, struct intel_dp_link *out)
{
    unsigned max_lanes = c->max_lanes < s->max_lanes ? c->max_lanes : s->max_lanes;
    if (!max_lanes || !c->n_rates || !m->clock_khz) return -1;
    unsigned bpps[2] = { 24, 18 }, nbpp = 2;
    if (edp && panel_bpp && panel_bpp < 24) { bpps[0] = 18; nbpp = 1; }   /* a 6 bpc panel */

    if (edp) {
        if (vbt_rate_khz > 0 && vbt_lanes && vbt_lanes <= max_lanes)
            for (unsigned i = 0; i < c->n_rates; i++)
                if (c->rates[i] == (uint32_t)vbt_rate_khz && usable(c, s, i) &&
                    (uint64_t)c->rates[i] * vbt_lanes >= need_kbytes(m, bpps[0])) {
                    fill(c, i, vbt_lanes, bpps[0], out);
                    return 0;
                }
        for (unsigned b = 0; b < nbpp; b++)
            for (int i = (int)c->n_rates - 1; i >= 0; i--)
                if (usable(c, s, (unsigned)i)) {
                    if ((uint64_t)c->rates[i] * max_lanes >= need_kbytes(m, bpps[b])) {
                        fill(c, (unsigned)i, max_lanes, bpps[b], out);
                        return 0;
                    }
                    break;                    /* the highest usable rate is the panel's */
                }
    }
    for (unsigned b = 0; b < nbpp; b++)
        for (unsigned i = 0; i < c->n_rates; i++) {
            if (!usable(c, s, i)) continue;
            for (unsigned lanes = 1; lanes <= max_lanes; lanes <<= 1)
                if ((uint64_t)c->rates[i] * lanes >= need_kbytes(m, bpps[b])) {
                    fill(c, i, lanes, bpps[b], out);
                    return 0;
                }
        }
    return -1;
}

int intel_dp_link_fallback(const struct intel_dp_caps *c, const struct intel_dp_source *s,
                           const struct display_mode *m, struct intel_dp_link *l)
{
    int cur = -1;
    for (unsigned i = 0; i < c->n_rates; i++) if (c->rates[i] == l->rate) cur = (int)i;
    if (cur < 0) return -1;
    /* A lower rate on the same lanes ... */
    for (int i = cur - 1; i >= 0; i--)
        if (usable(c, s, (unsigned)i) && (uint64_t)c->rates[i] * l->lanes >= need_kbytes(m, l->bpp)) {
            fill(c, (unsigned)i, l->lanes, l->bpp, l);
            return 0;
        }
    /* ... then half the lanes, from the top rate down. */
    for (unsigned lanes = l->lanes >> 1; lanes >= 1; lanes >>= 1)
        for (int i = (int)c->n_rates - 1; i >= 0; i--)
            if (usable(c, s, (unsigned)i) && (uint64_t)c->rates[i] * lanes >= need_kbytes(m, l->bpp)) {
                fill(c, (unsigned)i, lanes, l->bpp, l);
                return 0;
            }
    return -1;
}

/* i915 compute_m_n with a constant N (what Windows and the BIOS program), then
 * halved together until both fit the 24-bit registers. */
static void m_n(uint32_t *rm, uint32_t *rn, uint64_t m, uint64_t n, uint32_t constant_n)
{
    uint64_t mm = (m * constant_n) / n, nn = constant_n;
    while (mm > 0xffffffu || nn > 0xffffffu) { mm >>= 1; nn >>= 1; }
    *rm = (uint32_t)mm;
    *rn = (uint32_t)nn;
}

void intel_link_m_n(unsigned bpp, unsigned lanes, uint32_t pixel_khz, uint32_t link_khz, struct intel_m_n *o)
{
    o->tu = 64;
    m_n(&o->data_m, &o->data_n, (uint64_t)bpp * pixel_khz, (uint64_t)link_khz * lanes * 8, 0x8000000u);
    m_n(&o->link_m, &o->link_n, pixel_khz, link_khz, 0x80000u);
}

/* ---- PLLs and CDCLK --------------------------------------------------------- */
int intel_dpll_link_rate_code(uint32_t rate)
{
    switch (rate) {               /* DPLL_CTRL1_LINK_RATE_* */
    case 540000: return 0;        /* 2700 */
    case 270000: return 1;        /* 1350 */
    case 162000: return 2;        /* 810 */
    case 324000: return 3;        /* 1620 */
    case 216000: return 4;        /* 1080 */
    case 432000: return 5;        /* 2160 */
    default: return -1;
    }
}

struct wrpll_ctx { uint64_t min_dev, central, dco; unsigned p; };

static void wrpll_try(struct wrpll_ctx *c, uint64_t central, uint64_t dco, unsigned p)
{
    uint64_t diff = dco > central ? dco - central : central - dco;
    uint64_t dev = (10000u * diff) / central;          /* units of 0.01 % */
    if (dco >= central) {
        if (dev < 100 && dev < c->min_dev) { c->min_dev = dev; c->central = central; c->dco = dco; c->p = p; }
    } else if (dev < 600 && dev < c->min_dev) {
        c->min_dev = dev; c->central = central; c->dco = dco; c->p = p;
    }
}

int intel_wrpll_compute(uint32_t clock_khz, struct intel_wrpll *o)
{
    static const uint8_t even[] = { 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42, 44,
                                    48, 52, 54, 56, 60, 64, 66, 68, 70, 72, 76, 78, 80, 84, 88, 90,
                                    92, 96, 98 };
    static const uint8_t odd[] = { 3, 5, 7, 9, 15, 21, 35 };
    static const uint64_t central[3] = { 8400000000ull, 9000000000ull, 9600000000ull };
    if (!clock_khz || clock_khz > 600000) return -1;
    uint64_t afe = (uint64_t)clock_khz * 1000u * 5u;    /* AFE clock: 5x the TMDS clock, Hz */
    struct wrpll_ctx c = { ~0ull, 0, 0, 0 };
    for (unsigned d = 0; d < 2; d++) {
        const uint8_t *list = d == 0 ? even : odd;
        unsigned n = d == 0 ? sizeof even : sizeof odd;
        for (unsigned k = 0; k < 3; k++) {
            for (unsigned i = 0; i < n; i++) {
                wrpll_try(&c, central[k], list[i] * afe, list[i]);
                if (c.min_dev == 0) goto done;
            }
        }
    done:
        if (d == 0 && c.p) break;                       /* an even divider wins */
    }
    if (!c.p) return -1;

    unsigned p = c.p, p0 = 0, p1 = 0, p2 = 0;
    if (p % 2 == 0) {
        unsigned half = p / 2;
        if (half == 1 || half == 2 || half == 3 || half == 5) { p0 = 2; p1 = 1; p2 = half; }
        else if (half % 2 == 0) { p0 = 2; p1 = half / 2; p2 = 2; }
        else if (half % 3 == 0) { p0 = 3; p1 = half / 3; p2 = 2; }
        else if (half % 7 == 0) { p0 = 7; p1 = half / 7; p2 = 2; }
    } else if (p == 3 || p == 9) { p0 = 3; p1 = 1; p2 = p / 3; }
    else if (p == 5 || p == 7) { p0 = p; p1 = 1; p2 = 1; }
    else if (p == 15) { p0 = 3; p1 = 1; p2 = 5; }
    else if (p == 21) { p0 = 7; p1 = 1; p2 = 3; }
    else if (p == 35) { p0 = 7; p1 = 1; p2 = 5; }
    if (!p0) return -1;

    unsigned cf = c.central == 9600000000ull ? 0 : c.central == 9000000000ull ? 1 : 3;
    unsigned pdiv = p0 == 1 ? 0 : p0 == 2 ? 1 : p0 == 3 ? 2 : 4;
    unsigned kdiv = p2 == 5 ? 0 : p2 == 2 ? 1 : p2 == 3 ? 2 : 3;
    if ((p0 != 1 && p0 != 2 && p0 != 3 && p0 != 7) || (p2 != 5 && p2 != 2 && p2 != 3 && p2 != 1)) return -1;
    uint64_t dco = (uint64_t)p0 * p1 * p2 * afe;
    const uint64_t ref_khz = 24000;                     /* the SKL DPLL reference */
    uint64_t integer = dco / (ref_khz * 1000u);
    uint64_t fraction = ((dco / (ref_khz / 1000u) - integer * 1000000u) * 0x8000u) / 1000000u;
    o->dco_khz = (uint32_t)(dco / 1000u);
    o->p0 = (uint8_t)p0; o->p1 = (uint8_t)p1; o->p2 = (uint8_t)p2;
    o->cfgcr1 = (1u << 31) | (uint32_t)(fraction << 9) | (uint32_t)integer;
    o->cfgcr2 = ((uint32_t)p1 << 8) | ((p1 == 1 ? 0u : 1u) << 7) | (kdiv << 5) | (pdiv << 2) | cf;
    return 0;
}

uint32_t intel_wrpll_clock(uint32_t cfgcr1, uint32_t cfgcr2)
{
    static const unsigned pdiv[8] = { 1, 2, 3, 0, 7, 0, 0, 0 }, kdiv[4] = { 5, 2, 3, 1 };
    unsigned p0 = pdiv[(cfgcr2 >> 2) & 7], p1 = (cfgcr2 & 0x80) ? (cfgcr2 >> 8) & 0xff : 1;
    unsigned p2 = kdiv[(cfgcr2 >> 5) & 3];
    if (!p0 || !p1) return 0;
    uint64_t dco = (uint64_t)(cfgcr1 & 0x1ff) * 24000u + ((uint64_t)((cfgcr1 >> 9) & 0x7fff) * 24000u) / 0x8000u;
    return (uint32_t)(dco / ((uint64_t)p0 * p1 * p2 * 5u));
}

uint32_t intel_cdclk_pick(uint32_t min_khz, uint32_t vco_khz)
{
    static const uint32_t v8100[] = { 337500, 450000, 540000, 675000 };
    static const uint32_t v8640[] = { 308571, 432000, 540000, 617143 };
    const uint32_t *t = vco_khz == 8640000 ? v8640 : vco_khz == 8100000 ? v8100 : 0;
    for (unsigned i = 0; t && i < 4; i++) if (t[i] >= min_khz) return t[i];
    return 0;
}

uint32_t intel_cdclk_ctl(uint32_t cdclk)
{
    unsigned sel;
    switch (cdclk) {
    case 308571: case 337500: sel = 2; break;
    case 432000: case 450000: sel = 0; break;
    case 540000: sel = 1; break;
    default: sel = 3; break;                 /* 617143, 675000 */
    }
    return (sel << 26) | ((cdclk - 1000u + 250u) / 500u);
}

unsigned intel_cdclk_voltage_level(uint32_t cdclk)
{
    return cdclk > 540000 ? 3 : cdclk > 450000 ? 2 : cdclk > 337500 ? 1 : 0;
}

/* ---- display buffer and watermarks ------------------------------------------- */
void intel_ddb_split(unsigned npipes, const unsigned *hdisplay, struct intel_ddb *plane, struct intel_ddb *cursor)
{
    unsigned total = 0;
    for (unsigned i = 0; i < npipes; i++) total += hdisplay[i];
    unsigned before = 0, cur = npipes == 1 ? INTEL_CURSOR_DDB_SINGLE : INTEL_CURSOR_DDB_MULTI;
    for (unsigned i = 0; i < npipes; i++) {
        unsigned start = total ? INTEL_DDB_BLOCKS * before / total : 0;
        unsigned end = total ? INTEL_DDB_BLOCKS * (before + hdisplay[i]) / total : 0;
        before += hdisplay[i];
        cursor[i].start = (uint16_t)(end - cur); cursor[i].end = (uint16_t)end;
        plane[i].start = (uint16_t)start; plane[i].end = (uint16_t)(end - cur);
    }
}

void intel_wm_latency_adjust(uint32_t w0, uint32_t w1, uint16_t out[INTEL_WM_LEVELS])
{
    unsigned levels = INTEL_WM_LEVELS;
    for (unsigned i = 0; i < 4; i++) { out[i] = (w0 >> (8 * i)) & 0xff; out[4 + i] = (w1 >> (8 * i)) & 0xff; }
    for (unsigned l = 1; l < INTEL_WM_LEVELS; l++)
        if (out[l] == 0) { for (unsigned i = l; i < INTEL_WM_LEVELS; i++) out[i] = 0; levels = l; break; }
    if (out[0] == 0)                         /* WaWmMemoryReadLatency */
        for (unsigned l = 0; l < levels; l++) out[l] += 2;
    out[0] += 1;                             /* no DIMM information: the 16 GB DIMM workaround, as i915 assumes */
}

#define FX(v) ((uint64_t)(v) << 16)

int intel_plane_wm(const uint16_t lat[INTEL_WM_LEVELS], uint32_t pixel_khz, uint32_t htotal,
                   uint32_t width, uint32_t cpp, uint32_t ddb_blocks, struct intel_wm_level out[INTEL_WM_LEVELS])
{
    for (unsigned l = 0; l < INTEL_WM_LEVELS; l++) out[l] = (struct intel_wm_level){ 0, 0, 0 };
    if (!pixel_khz || !htotal || !width || !cpp) return -1;
    const uint32_t block = 512;
    uint32_t bytes_per_line = width * cpp;
    uint64_t bpl_fx = FX(DIV_ROUND_UP(bytes_per_line, block) + 1);      /* linear: +1 */
    uint32_t linetime_us = (uint32_t)DIV_ROUND_UP(DIV_ROUND_UP(FX((uint64_t)htotal * 1000u), pixel_khz), 1u << 16);
    int top = -1;
    uint16_t prev_blocks = 0;
    for (unsigned l = 0; l < INTEL_WM_LEVELS; l++) {
        if (l > 0 && lat[l] == 0) break;             /* sanitized away: this and every level above */
        uint32_t latency = lat[l];
        uint64_t method1 = DIV_ROUND_UP(FX((uint64_t)latency * pixel_khz * cpp), (uint64_t)1000 * block);
        uint64_t method2 = latency ? DIV_ROUND_UP((uint64_t)latency * pixel_khz, (uint64_t)htotal * 1000u) * bpl_fx
                                   : ~0ull;
        uint64_t sel;
        if ((uint64_t)cpp * htotal / block < 1 && bytes_per_line / block < 1) sel = method2;
        else if (latency >= linetime_us) sel = method1 < method2 ? method1 : method2;   /* Gen9: the smaller */
        else sel = method1;
        uint32_t blocks = (uint32_t)DIV_ROUND_UP(sel, 1u << 16) + 1;
        uint32_t lines = (uint32_t)DIV_ROUND_UP(sel, bpl_fx);
        if (l >= 1) {                                /* Display WA #1126 */
            blocks++;
            if (prev_blocks > blocks) blocks = prev_blocks;
        }
        if (l == 0) lines = 0;                       /* level 0 lines are ignored on Gen9 */
        if (lines > 31 || blocks + 1 > ddb_blocks) break;   /* does not fit: nor will anything above */
        out[l] = (struct intel_wm_level){ 1, (uint8_t)lines, (uint16_t)blocks };
        prev_blocks = (uint16_t)blocks;
        top = (int)l;
    }
    return top;
}

/* ---- the panel fitter ---------------------------------------------------------- */
static uint16_t scaler_phase(uint32_t scale)
{
    int phase = -0x8000 + (int)(scale / 2);          /* RGB: sub = 1, not chroma-cosited */
    uint16_t trip = 0;
    if (phase < 0) phase = 0x10000 + phase;
    else trip = 1;                                   /* PS_PHASE_TRIP */
    return (uint16_t)((((unsigned)phase >> 2) & (0x7fffu << 1)) | trip);
}

int intel_pfit_compute(unsigned sw, unsigned sh, unsigned dw, unsigned dh, struct intel_pfit *o)
{
    if (sw < 8 || sh < 8 || dw < 8 || dh < 8 || sw > 4096 || sh > 4096) return -1;
    if (sw > 2 * dw || sh > 2 * dh) return -1;
    unsigned w = dw, h = dh;
    uint64_t a = (uint64_t)sw * dh, b = (uint64_t)sh * dw;
    if (a > b) {                                     /* wider than the panel: bars top and bottom */
        h = (unsigned)((uint64_t)dw * sh / sw);
        h += h & 1;
        if (h > dh) h = dh;
    } else if (a < b) {                              /* narrower: bars left and right */
        w = (unsigned)((uint64_t)dh * sw / sh);
        w += w & 1;
        if (w > dw) w = dw;
    }
    o->w = (uint16_t)w; o->h = (uint16_t)h;
    o->x = (uint16_t)((dw - w) / 2); o->y = (uint16_t)((dh - h) / 2);
    o->hphase = scaler_phase((uint32_t)(((uint64_t)sw << 16) / w));
    o->vphase = scaler_phase((uint32_t)(((uint64_t)sh << 16) / h));
    return 0;
}
