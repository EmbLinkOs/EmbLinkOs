/* ui/backend/font.c -- EmbLink UI Piece 4b: the TrueType font rasterizer
 * (see font.h). Runtime glyf-outline parsing + supersampled scan conversion,
 * cached in a shared glyph atlas so each (font,codepoint,size) rasterizes once.
 *
 * Host-buildable; a freestanding ring-3 port needs malloc + a couple of libm
 * calls shimmed (same host-first posture as Pieces 3/4a). */

#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>    /* expf: the tracking curve, once per size */

/* ------------------------------------------------------------------------- */
/* big-endian sfnt readers (all bounds-checked against data_len by callers)   */
/* ------------------------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *d, uint32_t off) { return (uint16_t)((d[off] << 8) | d[off+1]); }
static int16_t  rd_i16(const uint8_t *d, uint32_t off) { return (int16_t)rd_u16(d, off); }
static uint32_t rd_u32(const uint8_t *d, uint32_t off) {
    return ((uint32_t)d[off] << 24) | ((uint32_t)d[off+1] << 16)
         | ((uint32_t)d[off+2] << 8) | (uint32_t)d[off+3];
}

/* libm-free floor/ceil so this file stays portable (freestanding-ready). */
static float floorf_(float v) { int i = (int)v; return (v < 0 && (float)i != v) ? (float)(i-1) : (float)i; }
static float ceilf_(float v)  { int i = (int)v; return (v > 0 && (float)i != v) ? (float)(i+1) : (float)i; }
static float sqrtf_(float v)  { if (v <= 0) return 0; float x = v; for (int i = 0; i < 8; i++) x = 0.5f*(x + v/x); return x; }
#define floorf floorf_
#define ceilf  ceilf_
#define sqrtf  sqrtf_

/* ------------------------------------------------------------------------- */
/* the small font table                                                      */
/* ------------------------------------------------------------------------- */

static struct font g_fonts[FONT_MAX];

struct font *font_for_handle(uint32_t handle) {
    if (handle == 0 || handle > FONT_MAX) return 0;
    struct font *f = &g_fonts[handle - 1];
    return f->used ? f : 0;
}

/* Locate a table's (offset,length) in the directory. Returns 0 if absent. */
static int find_table(const uint8_t *d, size_t len, const char tag[4],
                      uint32_t *out_off, uint32_t *out_len) {
    if (len < 12) return 0;
    uint16_t num_tables = rd_u16(d, 4);
    uint32_t dir = 12;
    for (uint16_t i = 0; i < num_tables; i++) {
        uint32_t rec = dir + (uint32_t)i * 16;
        if (rec + 16 > len) return 0;
        if (d[rec] == (uint8_t)tag[0] && d[rec+1] == (uint8_t)tag[1] &&
            d[rec+2] == (uint8_t)tag[2] && d[rec+3] == (uint8_t)tag[3]) {
            uint32_t off = rd_u32(d, rec + 8), tl = rd_u32(d, rec + 12);
            if ((uint64_t)off + tl > len) return 0;
            *out_off = off; *out_len = tl;
            return 1;
        }
    }
    return 0;
}

/* Walk cmap's encoding records for the Unicode subtable to use: a format-12
 * one (every plane -- Windows (3,10) first, then Unicode (0,4)/(0,6)) when
 * the font has it, else format 4 (the BMP; Windows (3,1)/(3,0) first).
 * Store its absolute offset and format. Returns 0 if none found. */
static int pick_cmap(const uint8_t *d, size_t len, uint32_t cmap_off, uint32_t *out_sub, uint16_t *out_format) {
    if (cmap_off + 4 > len) return 0;
    uint16_t n = rd_u16(d, cmap_off + 2);
    uint32_t best = 0; int best_rank = 0; uint16_t best_format = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t rec = cmap_off + 4 + (uint32_t)i * 8;
        if (rec + 8 > len) return 0;
        uint16_t plat = rd_u16(d, rec), enc = rd_u16(d, rec + 2);
        uint32_t sub = cmap_off + rd_u32(d, rec + 4);
        if (sub + 2 > len) continue;
        uint16_t format = rd_u16(d, sub);
        int rank = 0;
        if (format == 12) rank = plat == 3 && enc == 10 ? 6 : plat == 0 ? 5 : 4;
        else if (format == 4) rank = plat == 3 && (enc == 1 || enc == 0) ? 3 : plat == 0 ? 2 : 1;
        if (rank > best_rank) { best = sub; best_rank = rank; best_format = format; }
    }
    if (!best) return 0;
    *out_sub = best;
    *out_format = best_format;
    return 1;
}

/* ---- GPOS: the 'kern' feature's pair adjustments ------------------------ */

/* Every offset below is checked against the table's end: a face is a file,
 * and a broken one must kern nothing rather than read past itself. */
#define GP_OK(f_, o_, n_) ((uint64_t)(o_) + (n_) <= (uint64_t)(f_)->gpos_offset + (f_)->gpos_len)

static void gpos_kern_lookups(struct font *f) {
    const uint8_t *d = f->data;
    uint32_t g = f->gpos_offset;
    if (f->gpos_len < 10 || !GP_OK(f, g, 10)) return;
    uint32_t fl = g + rd_u16(d, g + 6);
    if (!GP_OK(f, fl, 2)) return;
    uint16_t nf = rd_u16(d, fl);
    for (uint16_t i = 0; i < nf && GP_OK(f, fl + 2 + 6u * i, 6); i++) {
        uint32_t rec = fl + 2 + 6u * i;
        if (d[rec] != 'k' || d[rec + 1] != 'e' || d[rec + 2] != 'r' || d[rec + 3] != 'n') continue;
        uint32_t ft = fl + rd_u16(d, rec + 4);
        if (!GP_OK(f, ft, 4)) continue;
        uint16_t n = rd_u16(d, ft + 2);
        for (uint16_t k = 0; k < n && GP_OK(f, ft + 4 + 2u * k, 2); k++) {
            uint16_t li = rd_u16(d, ft + 4 + 2u * k);
            int seen = 0;
            for (int m = 0; m < f->n_kern_lookups; m++) seen |= f->kern_lookups[m] == li;
            if (!seen && f->n_kern_lookups < 16) f->kern_lookups[f->n_kern_lookups++] = li;
        }
    }
}

/* Coverage: the glyph's index in the table, or -1. */
static int gp_coverage(struct font *f, uint32_t off, uint32_t gid) {
    const uint8_t *d = f->data;
    if (!GP_OK(f, off, 4)) return -1;
    uint16_t fmt = rd_u16(d, off), n = rd_u16(d, off + 2);
    int lo = 0, hi = n;
    if (fmt == 1) {
        if (!GP_OK(f, off + 4, 2u * n)) return -1;
        while (lo < hi) {
            int mid = (lo + hi) / 2; uint16_t gv = rd_u16(d, off + 4 + 2u * mid);
            if (gv == gid) return mid;
            if (gv < gid) lo = mid + 1; else hi = mid;
        }
    } else if (fmt == 2) {
        if (!GP_OK(f, off + 4, 6u * n)) return -1;
        while (lo < hi) {
            int mid = (lo + hi) / 2; uint32_t r = off + 4 + 6u * mid;
            uint16_t s = rd_u16(d, r), e = rd_u16(d, r + 2);
            if (gid < s) hi = mid;
            else if (gid > e) lo = mid + 1;
            else return rd_u16(d, r + 4) + (int)(gid - s);
        }
    }
    return -1;
}

/* ClassDef: the glyph's class, 0 when it has none. */
static int gp_class(struct font *f, uint32_t off, uint32_t gid) {
    const uint8_t *d = f->data;
    if (!GP_OK(f, off, 4)) return 0;
    uint16_t fmt = rd_u16(d, off);
    if (fmt == 1) {
        if (!GP_OK(f, off, 6)) return 0;
        uint16_t start = rd_u16(d, off + 2), n = rd_u16(d, off + 4);
        if (gid < start || gid >= (uint32_t)start + n || !GP_OK(f, off + 6, 2u * n)) return 0;
        return rd_u16(d, off + 6 + 2u * (gid - start));
    }
    if (fmt == 2) {
        uint16_t n = rd_u16(d, off + 2);
        if (!GP_OK(f, off + 4, 6u * n)) return 0;
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2; uint32_t r = off + 4 + 6u * mid;
            uint16_t s = rd_u16(d, r), e = rd_u16(d, r + 2);
            if (gid < s) hi = mid;
            else if (gid > e) lo = mid + 1;
            else return rd_u16(d, r + 4);
        }
    }
    return 0;
}

/* A ValueRecord's XAdvance (the bits before it say where it is), 0 if absent. */
static int gp_xadvance(struct font *f, uint32_t vr, uint16_t fmt) {
    if (!(fmt & 4) || !GP_OK(f, vr, 6)) return 0;
    return rd_i16(f->data, vr + 2u * ((fmt & 1) + ((fmt >> 1) & 1)));
}
static int gp_vr_size(uint16_t fmt) { int n = 0; for (int b = 0; b < 8; b++) n += (fmt >> b) & 1; return 2 * n; }

/* One PairPos subtable's adjustment for (g1, g2); *hit when it applies. */
static int gp_pairpos(struct font *f, uint32_t st, uint32_t g1, uint32_t g2, int *hit) {
    const uint8_t *d = f->data;
    if (!GP_OK(f, st, 10)) return 0;
    uint16_t fmt = rd_u16(d, st), vf1 = rd_u16(d, st + 4), vf2 = rd_u16(d, st + 6);
    int ci = gp_coverage(f, st + rd_u16(d, st + 2), g1);
    if (ci < 0) return 0;
    int s1 = gp_vr_size(vf1), s2 = gp_vr_size(vf2);
    if (fmt == 1) {
        if (ci >= rd_u16(d, st + 8) || !GP_OK(f, st + 10 + 2u * ci, 2)) return 0;
        uint32_t ps = st + rd_u16(d, st + 10 + 2u * ci);
        if (!GP_OK(f, ps, 2)) return 0;
        uint16_t n = rd_u16(d, ps);
        uint32_t rec = 2 + (uint32_t)s1 + (uint32_t)s2;
        if (!GP_OK(f, ps + 2, rec * n)) return 0;
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = (lo + hi) / 2; uint32_t r = ps + 2 + rec * (uint32_t)mid;
            uint16_t sg = rd_u16(d, r);
            if (sg == g2) { *hit = 1; return gp_xadvance(f, r + 2, vf1); }
            if (sg < g2) lo = mid + 1; else hi = mid;
        }
        return 0;
    }
    if (fmt == 2 && GP_OK(f, st, 16)) {
        uint16_t c1n = rd_u16(d, st + 12), c2n = rd_u16(d, st + 14);
        int c1 = gp_class(f, st + rd_u16(d, st + 8), g1), c2 = gp_class(f, st + rd_u16(d, st + 10), g2);
        if (c1 >= c1n || c2 >= c2n) return 0;
        *hit = 1;
        return gp_xadvance(f, st + 16 + (uint32_t)(c1 * c2n + c2) * (uint32_t)(s1 + s2), vf1);
    }
    return 0;
}

/* The pair's kerning in font units: each 'kern' lookup's first subtable that
 * applies, summed over the lookups (extension subtables followed). */
static int gp_kern_units(struct font *f, uint32_t g1, uint32_t g2) {
    const uint8_t *d = f->data;
    uint32_t g = f->gpos_offset, ll = g + rd_u16(d, g + 8);
    if (!g1 || !g2 || !GP_OK(f, ll, 2)) return 0;
    uint16_t nl = rd_u16(d, ll);
    int total = 0;
    for (int k = 0; k < f->n_kern_lookups; k++) {
        uint16_t li = f->kern_lookups[k];
        if (li >= nl || !GP_OK(f, ll + 2 + 2u * li, 2)) continue;
        uint32_t lk = ll + rd_u16(d, ll + 2 + 2u * li);
        if (!GP_OK(f, lk, 6)) continue;
        uint16_t type = rd_u16(d, lk), ns = rd_u16(d, lk + 4);
        for (uint16_t s = 0; s < ns && GP_OK(f, lk + 6 + 2u * s, 2); s++) {
            uint32_t st = lk + rd_u16(d, lk + 6 + 2u * s);
            uint16_t t = type;
            if (t == 9 && GP_OK(f, st, 8)) { t = rd_u16(d, st + 2); st += rd_u32(d, st + 4); }
            if (t != 2) continue;
            int hit = 0, v = gp_pairpos(f, st, g1, g2, &hit);
            if (hit) { total += v; break; }
        }
    }
    return total;
}

float font_kern_px(struct font *f, uint32_t left, uint32_t right, float size_px) {
    if (!f || !left || !right || !f->n_kern_lookups) return 0.0f;
    int units;
    if (left >= 0x20 && left < 0x7F && right >= 0x20 && right < 0x7F) {
        if (!f->kern_ascii) {
            /* every printable ASCII pair, once: 9025 lookups, then a load */
            f->kern_ascii = (int16_t *)malloc(95 * 95 * sizeof(int16_t));
            if (!f->kern_ascii) return 0.0f;
            uint32_t gid[95];
            for (int i = 0; i < 95; i++) gid[i] = font_codepoint_to_glyph(f, (uint32_t)(0x20 + i));
            for (int a = 0; a < 95; a++)
                for (int b = 0; b < 95; b++) f->kern_ascii[a * 95 + b] = (int16_t)gp_kern_units(f, gid[a], gid[b]);
        }
        units = f->kern_ascii[(left - 0x20) * 95 + (right - 0x20)];
    } else {
        if (font_for_codepoint(f, left) != f || font_for_codepoint(f, right) != f) return 0.0f;
        units = gp_kern_units(f, font_codepoint_to_glyph(f, left), font_codepoint_to_glyph(f, right));
    }
    float canonical = (float)(uint16_t)(size_px * 4.0f + .5f) / 4.0f;   /* the cache's size */
    return (float)units * canonical / (float)f->units_per_em;
}

/* Inter's dynamic metrics (rsms.me/inter/dynmetrics): tracking in ems as a
 * function of the size in pixels -- about +0.005 em at 11 px, -0.003 at 13,
 * -0.02 at 24 and up. */
static float inter_tracking_em(float size_px) { return -0.0223f + 0.185f * expf(-0.1745f * size_px); }

float font_pair_px(struct font *f, uint32_t left, uint32_t right, float size_px) {
    if (!f || !left || !right) return 0.0f;
    float px = font_kern_px(f, left, right, size_px);
    if (f->dyn_tracking) {
        float canonical = (float)(uint16_t)(size_px * 4.0f + .5f) / 4.0f;   /* the cache's size */
        if (canonical != f->trk_size) { f->trk_size = canonical; f->trk_px = inter_tracking_em(canonical) * canonical; }
        px += f->trk_px;
    }
    return px;
}

/* Is the face's family (name ID 1, Windows UTF-16 or Mac Roman) `family`? */
static int family_is(const uint8_t *d, size_t len, const char *family) {
    uint32_t off, tl;
    if (!find_table(d, len, "name", &off, &tl) || tl < 6) return 0;
    uint16_t count = rd_u16(d, off + 2), strings = rd_u16(d, off + 4);
    size_t fl = strlen(family);
    for (uint16_t i = 0; i < count && 6u + 12u * (i + 1u) <= tl; i++) {
        uint32_t r = off + 6 + 12u * i;
        uint16_t platform = rd_u16(d, r), name_id = rd_u16(d, r + 6), slen = rd_u16(d, r + 8), soff = rd_u16(d, r + 10);
        if (name_id != 1 || (size_t)off + strings + soff + slen > len) continue;
        const uint8_t *str = d + off + strings + soff;
        if (platform == 3 && slen >= 2 * fl) {
            size_t k = 0;
            while (k < fl && str[2 * k] == 0 && str[2 * k + 1] == (uint8_t)family[k]) k++;
            if (k == fl) return 1;
        } else if (platform == 1 && slen >= fl && !memcmp(str, family, fl)) return 1;
    }
    return 0;
}

uint32_t font_load(const uint8_t *ttf_data, size_t len) {
    if (!ttf_data || len < 12) return 0;
    uint32_t ver = rd_u32(ttf_data, 0);
    if (ver != 0x00010000 && ver != 0x74727565 /* 'true' */) return 0;

    int slot = -1;
    for (int i = 0; i < FONT_MAX; i++) if (!g_fonts[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    struct font *f = &g_fonts[slot];
    memset(f, 0, sizeof(*f));
    f->data = ttf_data; f->data_len = len;

    uint32_t off, tl;
    if (!find_table(ttf_data, len, "head", &off, &tl) || tl < 54) return 0;
    f->units_per_em = rd_u16(ttf_data, off + 18);
    f->index_to_loc_fmt = rd_i16(ttf_data, off + 50);
    if (f->units_per_em == 0) return 0;

    if (!find_table(ttf_data, len, "maxp", &off, &tl) || tl < 6) return 0;
    f->num_glyphs = rd_u16(ttf_data, off + 4);

    if (!find_table(ttf_data, len, "hhea", &off, &tl) || tl < 36) return 0;
    f->ascent   = rd_i16(ttf_data, off + 4);
    f->descent  = (int16_t)(-rd_i16(ttf_data, off + 6));  /* file stores it negative */
    f->line_gap = rd_i16(ttf_data, off + 8);
    f->num_h_metrics = rd_u16(ttf_data, off + 34);

    if (!find_table(ttf_data, len, "hmtx", &off, &tl)) return 0;
    f->hmtx_offset = off;

    if (!find_table(ttf_data, len, "loca", &f->loca_offset, &f->loca_len)) return 0;
    if (!find_table(ttf_data, len, "glyf", &f->glyf_offset, &f->glyf_len)) return 0;

    uint32_t cmap_off, cmap_len;
    if (!find_table(ttf_data, len, "cmap", &cmap_off, &cmap_len)) return 0;
    if (!pick_cmap(ttf_data, len, cmap_off, &f->cmap_subtable_offset, &f->cmap_format)) return 0;

    if (find_table(ttf_data, len, "GPOS", &f->gpos_offset, &f->gpos_len)) gpos_kern_lookups(f);
    f->dyn_tracking = family_is(ttf_data, len, "Inter");

    f->used = true;
    return (uint32_t)(slot + 1);
}

/* ------------------------------------------------------------------------- */
/* Section 2: cmap formats 12 and 4                                          */
/* ------------------------------------------------------------------------- */

/* Format 12: groups of (first, last, first glyph), sorted -- a binary search. */
static uint32_t cmap12_glyph(struct font *f, uint32_t codepoint) {
    const uint8_t *d = f->data;
    uint32_t s = f->cmap_subtable_offset;
    if (s + 16 > f->data_len) return 0;
    uint32_t groups = rd_u32(d, s + 12);
    if ((uint64_t)s + 16 + (uint64_t)groups * 12 > f->data_len) return 0;
    uint32_t lo = 0, hi = groups;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, g = s + 16 + mid * 12;
        uint32_t first = rd_u32(d, g), last = rd_u32(d, g + 4);
        if (codepoint < first) hi = mid;
        else if (codepoint > last) lo = mid + 1;
        else {
            uint32_t glyph = rd_u32(d, g + 8) + (codepoint - first);
            return glyph < f->num_glyphs ? glyph : 0;
        }
    }
    return 0;
}

void font_set_fallback(uint32_t font, uint32_t fallback) {
    struct font *f = font_for_handle(font);
    if (f && fallback != font) f->fallback = fallback;
}

struct font *font_for_codepoint(struct font *f, uint32_t codepoint) {
    if (!f || codepoint < 0x80 || font_codepoint_to_glyph(f, codepoint)) return f;
    struct font *fb = font_for_handle(f->fallback);
    return fb && font_codepoint_to_glyph(fb, codepoint) ? fb : f;
}

bool font_has_glyph(struct font *f, uint32_t codepoint) {
    return font_codepoint_to_glyph(f, codepoint) != 0;
}

uint32_t font_codepoint_to_glyph(struct font *f, uint32_t codepoint) {
    if (!f) return 0;
    if (f->cmap_format == 12) return cmap12_glyph(f, codepoint);
    if (codepoint > 0xFFFF) return 0;
    const uint8_t *d = f->data;
    uint32_t s = f->cmap_subtable_offset;
    if (s + 14 > f->data_len) return 0;
    uint16_t segX2 = rd_u16(d, s + 6);
    uint16_t segs = segX2 / 2;
    uint32_t endCode   = s + 14;
    uint32_t startCode = endCode + segX2 + 2;   /* +2 reservedPad */
    uint32_t idDelta   = startCode + segX2;
    uint32_t idRange   = idDelta + segX2;
    if (idRange + segX2 > f->data_len) return 0;

    /* endCode[] is sorted: the segment is the first whose end >= codepoint */
    uint16_t lo = 0, hi = segs;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        if (rd_u16(d, endCode + mid * 2) < codepoint) lo = (uint16_t)(mid + 1); else hi = mid;
    }
    if (lo < segs) {
        uint16_t i = lo;
        uint16_t start = rd_u16(d, startCode + i * 2);
        if (codepoint < start) return 0;         /* in a gap */
        int16_t delta = rd_i16(d, idDelta + i * 2);
        uint16_t ro = rd_u16(d, idRange + i * 2);
        if (ro == 0) return (uint16_t)(codepoint + delta);
        /* indirect: &glyphIdArray[...] = idRangeOffset addr + ro + 2*(cp-start) */
        uint32_t addr = idRange + i * 2 + ro + (uint32_t)(codepoint - start) * 2;
        if (addr + 2 > f->data_len) return 0;
        uint16_t g = rd_u16(d, addr);
        if (g == 0) return 0;
        return (uint16_t)(g + delta);
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Section 3: glyf/loca outline extraction                                   */
/* ------------------------------------------------------------------------- */

static int loca_range(struct font *f, uint32_t gi, uint32_t *g_off, uint32_t *g_end) {
    const uint8_t *d = f->data;
    if (gi + 1 > f->num_glyphs) return 0;
    if (f->index_to_loc_fmt == 0) {
        uint32_t a = f->loca_offset + gi * 2;
        if (a + 4 > f->data_len) return 0;
        *g_off = (uint32_t)rd_u16(d, a) * 2;
        *g_end = (uint32_t)rd_u16(d, a + 2) * 2;
    } else {
        uint32_t a = f->loca_offset + gi * 4;
        if (a + 8 > f->data_len) return 0;
        *g_off = rd_u32(d, a);
        *g_end = rd_u32(d, a + 4);
    }
    return 1;
}

static uint16_t glyph_advance(struct font *f, uint32_t gi) {
    uint32_t n = f->num_h_metrics;
    if (n == 0) return 0;
    uint32_t idx = gi < n ? gi : n - 1;
    uint32_t a = f->hmtx_offset + idx * 4;
    if (a + 2 > f->data_len) return 0;
    return rd_u16(f->data, a);
}

/* TrueType simple-glyph flag bits */
#define GF_ON_CURVE  0x01
#define GF_X_SHORT   0x02
#define GF_Y_SHORT   0x04
#define GF_REPEAT    0x08
#define GF_X_SAME    0x10   /* if X_SHORT: x is positive; else x delta is 0 */
#define GF_Y_SAME    0x20

static bool parse_simple_glyph(struct font *f, uint32_t goff, int16_t ncont,
                               struct glyph_outline *out) {
    const uint8_t *d = f->data;
    uint32_t p = goff + 10;
    if (p + (uint32_t)ncont * 2 + 2 > f->data_len) return false;

    uint16_t *ends = (uint16_t *)malloc(sizeof(uint16_t) * ncont);
    if (!ends) return false;
    for (int i = 0; i < ncont; i++) { ends[i] = rd_u16(d, p); p += 2; }
    uint32_t n_points = (uint32_t)ends[ncont - 1] + 1;

    uint16_t instr_len = rd_u16(d, p); p += 2;
    p += instr_len;                             /* skip hinting bytecode (Section 0) */
    if (p > f->data_len) { free(ends); return false; }

    uint8_t *flags = (uint8_t *)malloc(n_points);
    if (!flags) { free(ends); return false; }
    for (uint32_t i = 0; i < n_points; ) {
        if (p >= f->data_len) { free(ends); free(flags); return false; }
        uint8_t fl = d[p++];
        flags[i++] = fl;
        if (fl & GF_REPEAT) {
            if (p >= f->data_len) { free(ends); free(flags); return false; }
            uint8_t rep = d[p++];
            while (rep-- && i < n_points) flags[i++] = fl;
        }
    }

    float *xs = (float *)malloc(sizeof(float) * n_points);
    float *ys = (float *)malloc(sizeof(float) * n_points);
    if (!xs || !ys) { free(ends); free(flags); free(xs); free(ys); return false; }

    int32_t x = 0;
    for (uint32_t i = 0; i < n_points; i++) {
        uint8_t fl = flags[i];
        if (fl & GF_X_SHORT) {
            if (p >= f->data_len) break;
            uint8_t dx = d[p++];
            x += (fl & GF_X_SAME) ? (int32_t)dx : -(int32_t)dx;
        } else if (!(fl & GF_X_SAME)) {
            if (p + 2 > f->data_len) break;
            x += rd_i16(d, p); p += 2;
        }
        xs[i] = (float)x;
    }
    int32_t y = 0;
    for (uint32_t i = 0; i < n_points; i++) {
        uint8_t fl = flags[i];
        if (fl & GF_Y_SHORT) {
            if (p >= f->data_len) break;
            uint8_t dy = d[p++];
            y += (fl & GF_Y_SAME) ? (int32_t)dy : -(int32_t)dy;
        } else if (!(fl & GF_Y_SAME)) {
            if (p + 2 > f->data_len) break;
            y += rd_i16(d, p); p += 2;
        }
        ys[i] = (float)y;
    }

    out->n_contours = (uint32_t)ncont;
    out->contours = (struct glyph_contour *)calloc(ncont, sizeof(struct glyph_contour));
    if (!out->contours) { free(ends); free(flags); free(xs); free(ys); return false; }
    uint32_t start = 0;
    for (int c = 0; c < ncont; c++) {
        uint32_t cend = ends[c];
        uint32_t cnt = cend - start + 1;
        struct glyph_contour *ct = &out->contours[c];
        ct->points = (struct glyph_point *)malloc(sizeof(struct glyph_point) * cnt);
        ct->n_points = cnt;
        for (uint32_t k = 0; k < cnt; k++) {
            ct->points[k].x = xs[start + k];
            ct->points[k].y = ys[start + k];
            ct->points[k].on_curve = (flags[start + k] & GF_ON_CURVE) != 0;
        }
        start = cend + 1;
    }

    free(ends); free(flags); free(xs); free(ys);
    return true;
}

/* composite-glyph flag bits */
#define CG_ARG_WORDS     0x0001
#define CG_ARGS_XY       0x0002
#define CG_HAVE_SCALE    0x0008
#define CG_MORE          0x0020
#define CG_XY_SCALE      0x0040
#define CG_2X2           0x0080

static bool parse_glyph_recursive(struct font *f, uint32_t gi, struct glyph_outline *out, int depth);

static bool parse_composite_glyph(struct font *f, uint32_t goff, struct glyph_outline *out, int depth) {
    const uint8_t *d = f->data;
    uint32_t p = goff + 10;
    /* accumulate components' contours into a growing array */
    out->n_contours = 0; out->contours = 0;

    for (;;) {
        if (p + 4 > f->data_len) return false;
        uint16_t flags = rd_u16(d, p); p += 2;
        uint16_t comp_gi = rd_u16(d, p); p += 2;

        float dx, dy;
        if (flags & CG_ARG_WORDS) {
            if (p + 4 > f->data_len) return false;
            dx = (float)rd_i16(d, p); dy = (float)rd_i16(d, p + 2); p += 4;
        } else {
            if (p + 2 > f->data_len) return false;
            dx = (float)(int8_t)d[p]; dy = (float)(int8_t)d[p + 1]; p += 2;
        }
        float sx = 1.0f, sy = 1.0f;
        if (flags & CG_HAVE_SCALE) {
            if (p + 2 > f->data_len) return false;
            sx = sy = rd_i16(d, p) / 16384.0f; p += 2;
        } else if (flags & CG_XY_SCALE) {
            if (p + 4 > f->data_len) return false;
            sx = rd_i16(d, p) / 16384.0f; sy = rd_i16(d, p + 2) / 16384.0f; p += 4;
        } else if (flags & CG_2X2) {
            if (p + 8 > f->data_len) return false;
            sx = rd_i16(d, p) / 16384.0f; sy = rd_i16(d, p + 6) / 16384.0f; p += 8; /* diag only (Section 0) */
        }

        struct glyph_outline comp;
        memset(&comp, 0, sizeof(comp));
        if (parse_glyph_recursive(f, comp_gi, &comp, depth + 1) && comp.n_contours > 0) {
            uint32_t base = out->n_contours;
            uint32_t total = base + comp.n_contours;
            struct glyph_contour *nc = (struct glyph_contour *)realloc(out->contours,
                                       sizeof(struct glyph_contour) * total);
            if (nc) {
                out->contours = nc;
                for (uint32_t c = 0; c < comp.n_contours; c++) {
                    struct glyph_contour *src = &comp.contours[c];
                    struct glyph_contour *dstc = &out->contours[base + c];
                    dstc->n_points = src->n_points;
                    dstc->points = (struct glyph_point *)malloc(sizeof(struct glyph_point) * src->n_points);
                    for (uint32_t k = 0; k < src->n_points; k++) {
                        dstc->points[k].x = src->points[k].x * sx + dx;
                        dstc->points[k].y = src->points[k].y * sy + dy;
                        dstc->points[k].on_curve = src->points[k].on_curve;
                    }
                }
                out->n_contours = total;
            }
        }
        font_free_outline(&comp);
        if (!(flags & CG_MORE)) break;
    }
    return true;
}

static bool parse_glyph_recursive(struct font *f, uint32_t gi, struct glyph_outline *out, int depth) {
    if (depth > 5) return false;   /* guard against pathological composite cycles */
    memset(out, 0, sizeof(*out));
    uint32_t goff, gend;
    if (!loca_range(f, gi, &goff, &gend)) return false;
    out->advance_width = glyph_advance(f, gi);
    if (gend <= goff) return true;   /* empty glyph (e.g. space) -- valid, 0 contours */

    uint32_t abs_off = f->glyf_offset + goff;
    if (abs_off + 10 > f->data_len) return false;
    int16_t ncont = rd_i16(f->data, abs_off);
    out->x_min = rd_i16(f->data, abs_off + 2);
    out->y_min = rd_i16(f->data, abs_off + 4);
    out->x_max = rd_i16(f->data, abs_off + 6);
    out->y_max = rd_i16(f->data, abs_off + 8);

    if (ncont >= 0) return parse_simple_glyph(f, abs_off, ncont, out);
    return parse_composite_glyph(f, abs_off, out, depth);
}

bool font_get_glyph_outline(struct font *f, uint32_t glyph_index, struct glyph_outline *out) {
    if (!f || !out) return false;
    return parse_glyph_recursive(f, glyph_index, out, 0);
}

void font_free_outline(struct glyph_outline *o) {
    if (!o || !o->contours) return;
    for (uint32_t i = 0; i < o->n_contours; i++) free(o->contours[i].points);
    free(o->contours);
    o->contours = 0; o->n_contours = 0;
}

/* ------------------------------------------------------------------------- */
/* Section 4: rasterization (exact area coverage)                            */
/* ------------------------------------------------------------------------- */


static uint32_t g_rasterize_calls;
uint32_t font_debug_rasterize_count(void) { return g_rasterize_calls; }

struct fpt { float x, y; };

/* Flatten one contour (font-unit points) into a closed pixel-space, y-down
 * polyline, reconstructing implicit on-curve midpoints (Section 3). Appends to
 * *poly. Algorithm: rotate the ring to begin on an on-curve anchor (synthesize
 * one if the contour is all off-curve), then walk emitting a straight segment
 * to each on-curve point and a 12-step quadratic Bezier through each control
 * point, materializing the implicit midpoint when two controls are adjacent. */
static void flatten_contour(const struct glyph_point *pts, uint32_t n,
                            float scale, struct fpt **poly, int *np, int *cap) {
    if (n == 0) return;

    /* build a rotated working ring E[] that STARTS on an on-curve point */
    struct fpt *E = (struct fpt *)malloc(sizeof(struct fpt) * (n + 1));
    bool *on = (bool *)malloc(sizeof(bool) * (n + 1));
    uint32_t m = 0;
    int s = -1;
    for (uint32_t i = 0; i < n; i++) if (pts[i].on_curve) { s = (int)i; break; }
    if (s >= 0) {
        for (uint32_t k = 0; k < n; k++) {
            uint32_t i = (uint32_t)s + k; if (i >= n) i -= n;
            E[m].x = pts[i].x * scale; E[m].y = -pts[i].y * scale; on[m] = pts[i].on_curve; m++;
        }
    } else {
        /* no on-curve: synthesize an anchor at the mid of last & first */
        E[m].x = (pts[n-1].x + pts[0].x) * 0.5f * scale;
        E[m].y = -(pts[n-1].y + pts[0].y) * 0.5f * scale; on[m] = true; m++;
        for (uint32_t k = 0; k < n; k++) {
            E[m].x = pts[k].x * scale; E[m].y = -pts[k].y * scale; on[m] = false; m++;
        }
    }

    #define PUSH(px,py) do { \
        if (*np >= *cap) { *cap = *cap ? *cap*2 : 64; *poly = (struct fpt*)realloc(*poly, sizeof(struct fpt)*(*cap)); } \
        (*poly)[*np].x = (px); (*poly)[*np].y = (py); (*np)++; } while (0)

    struct fpt anchor = E[0], cur = anchor;
    PUSH(anchor.x, anchor.y);
    uint32_t j = 1;
    while (j <= m) {
        struct fpt p = (j < m) ? E[j] : anchor;   /* j==m closes back to the anchor */
        int p_on = (j < m) ? on[j] : 1;
        if (p_on) {
            PUSH(p.x, p.y);                        /* straight segment */
            cur = p; j++;
        } else {
            struct fpt ctrl = p, end;
            uint32_t nj = j + 1;
            struct fpt np2 = (nj < m) ? E[nj] : anchor;
            int n_on = (nj < m) ? on[nj] : 1;
            if (n_on) { end = np2; }
            else { end.x = (ctrl.x + np2.x) * 0.5f; end.y = (ctrl.y + np2.y) * 0.5f; }
            for (int st = 1; st <= 12; st++) {     /* fixed 12-step quadratic */
                float t = st / 12.0f, u = 1.0f - t;
                float bx = u*u*cur.x + 2*u*t*ctrl.x + t*t*end.x;
                float by = u*u*cur.y + 2*u*t*ctrl.y + t*t*end.y;
                PUSH(bx, by);
            }
            cur = end;
            j += n_on ? 2 : 1;                     /* consumed the explicit end too */
        }
    }
    #undef PUSH
    free(E); free(on);
}

int font_debug_flatten_contour0(struct font *f, uint32_t gi, float scale,
                                float *xs, float *ys, int max) {
    struct glyph_outline o;
    if (!font_get_glyph_outline(f, gi, &o) || o.n_contours == 0) { font_free_outline(&o); return 0; }
    struct fpt *poly = 0; int np = 0, cap = 0;
    flatten_contour(o.contours[0].points, o.contours[0].n_points, scale, &poly, &np, &cap);
    int cnt = np < max ? np : max;
    for (int i = 0; i < cnt; i++) { xs[i] = poly[i].x; ys[i] = poly[i].y; }
    free(poly); font_free_outline(&o);
    return np;
}

/* One edge, (x0,y0)-(x1,y1) in the glyph's own pixel space (y down), into
 * the accumulation buffer `a` (`aw` floats a row, two more than the glyph is
 * wide). Per row it crosses: the signed height it covers there, split between
 * the cells its x span touches by the area to the right of the edge. */
static void area_line(float *a, int aw, int h, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1.0f;
    if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1.0f; }
    float dxdy = (x1 - x0) / (y1 - y0), x = x0;
    if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
    int yend = (int)ceilf_(y1);
    if (yend > h) yend = h;
    for (int y = (int)y0; y < yend; y++) {
        float top = (float)y > y0 ? (float)y : y0;
        float bot = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = bot - top;
        if (dy <= 0) continue;
        float xnext = x + dxdy * dy, d = dy * dir;
        float xa = x < xnext ? x : xnext, xb = x < xnext ? xnext : x;
        if (xa < 0) xa = 0;                       /* outside the box is clamped in */
        if (xb < 0) xb = 0;
        float *row = a + (size_t)y * aw;
        float xaf = floorf_(xa);
        int ia = (int)xaf, ib = (int)ceilf_(xb);
        if (ia >= aw - 1) ia = aw - 2;
        if (ib <= ia + 1) {                       /* within one cell */
            float xm = 0.5f * (xa + xb) - xaf;
            row[ia] += d - d * xm;
            row[ia + 1] += d * xm;
        } else {                                  /* across several */
            float s = 1.0f / (xb - xa), f0 = xa - xaf;
            float a0 = 0.5f * s * (1.0f - f0) * (1.0f - f0);
            float f1 = xb - (float)ib + 1.0f, am = 0.5f * s * f1 * f1;
            if (ib > aw - 1) ib = aw - 1;
            row[ia] += d * a0;
            if (ib == ia + 2) row[ia + 1] += d * (1.0f - a0 - am);
            else {
                float a1 = s * (1.5f - f0);
                row[ia + 1] += d * (a1 - a0);
                for (int k = ia + 2; k < ib - 1; k++) row[k] += d * s;
                float a2 = a1 + (float)(ib - ia - 3) * s;
                row[ib - 1] += d * (1.0f - a2 - am);
            }
            row[ib] += d * am;
        }
        x = xnext;
    }
}

bool font_rasterize_glyph(struct font *f, uint32_t glyph_index, float size_px,
                          uint8_t **out_coverage, int *out_w, int *out_h,
                          int *out_bearing_x, int *out_bearing_y, float *out_advance_px) {
    return font_rasterize_glyph_at(f, glyph_index, size_px, 0.0f, out_coverage, out_w, out_h,
                                   out_bearing_x, out_bearing_y, out_advance_px);
}

bool font_rasterize_glyph_at(struct font *f, uint32_t glyph_index, float size_px, float dx,
                             uint8_t **out_coverage, int *out_w, int *out_h,
                             int *out_bearing_x, int *out_bearing_y, float *out_advance_px) {
    g_rasterize_calls++;
    struct glyph_outline o;
    if (!font_get_glyph_outline(f, glyph_index, &o)) return false;

    float scale = size_px / (float)f->units_per_em;
    *out_advance_px = o.advance_width * scale;

    /* flatten every contour into one polyline list, tracking contour spans */
    uint32_t ncont = o.n_contours;   /* SAVE before free -- the scanline loop
                                      * below still needs the count, and
                                      * font_free_outline zeroes o.n_contours */
    struct fpt *poly = 0; int np = 0, cap = 0;
    int *span_end = (int *)malloc(sizeof(int) * (ncont ? ncont : 1));
    for (uint32_t c = 0; c < ncont; c++) {
        flatten_contour(o.contours[c].points, o.contours[c].n_points, scale, &poly, &np, &cap);
        span_end[c] = np;   /* this contour's points are [prev_end, np) */
    }
    font_free_outline(&o);
    if (dx != 0.0f) for (int i = 0; i < np; i++) poly[i].x += dx;   /* the pen's fraction */

    if (np == 0) {   /* blank glyph (space): valid, empty bitmap */
        free(poly); free(span_end);
        *out_coverage = 0; *out_w = 0; *out_h = 0; *out_bearing_x = 0; *out_bearing_y = 0;
        return true;
    }

    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < np; i++) {
        if (poly[i].x < minx) minx = poly[i].x;
        if (poly[i].x > maxx) maxx = poly[i].x;
        if (poly[i].y < miny) miny = poly[i].y;
        if (poly[i].y > maxy) maxy = poly[i].y;
    }
    int ox = (int)floorf(minx), oy = (int)floorf(miny);
    int w = (int)ceilf(maxx) - ox;
    int h = (int)ceilf(maxy) - oy;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    /* EXACT AREA COVERAGE. Each edge adds, to every pixel it crosses, the
     * signed area it sweeps there; a running sum along the row then is the
     * winding-weighted area of the outline inside each pixel -- 256 levels,
     * exact for straight edges, where 4 x 4 supersampling gave 17 and lost
     * whatever fell between its samples (the font-rs / stb_truetype 2
     * method). Clamped to 1, which is nonzero winding for outlines that do
     * not cover a pixel twice and the right answer for those that do. */
    int aw = w + 2;
    float *acc = (float *)calloc((size_t)aw * (size_t)h + 1, sizeof(float));
    if (!acc) { free(poly); free(span_end); return false; }
    int cstart = 0;
    for (uint32_t c = 0; c < ncont; c++) {
        int cend = span_end[c];
        for (int i = cstart; i < cend; i++) {
            int j = (i + 1 < cend) ? i + 1 : cstart;   /* close the contour */
            area_line(acc, aw, h, poly[i].x - (float)ox, poly[i].y - (float)oy,
                      poly[j].x - (float)ox, poly[j].y - (float)oy);
        }
        cstart = cend;
    }
    uint8_t *cov = (uint8_t *)malloc((size_t)w * h);
    if (!cov) { free(acc); free(poly); free(span_end); return false; }
    for (int py = 0; py < h; py++) {
        float sum = 0.0f;
        for (int px = 0; px < w; px++) {
            sum += acc[py * aw + px];
            float a = sum < 0 ? -sum : sum;
            cov[py * w + px] = (uint8_t)(a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f));
        }
    }
    free(acc);

    free(poly); free(span_end);
    *out_coverage = cov; *out_w = w; *out_h = h;
    *out_bearing_x = ox;
    *out_bearing_y = -oy;   /* pixels from baseline up to glyph top (oy is negative, y-down) */
    return true;
}

/* ------------------------------------------------------------------------- */
/* Section 5: glyph atlas                                                     */
/* ------------------------------------------------------------------------- */

static struct glyph_atlas g_atlas;
struct glyph_atlas *font_global_atlas(void) { return &g_atlas; }

const uint8_t *glyph_coverage(const struct glyph_atlas *a, const struct glyph_cache_entry *e) {
    if (!e || e->page >= GLYPH_ATLAS_PAGES || !e->atlas_w || !a->pages[e->page].coverage) return 0;
    return a->pages[e->page].coverage + (size_t)e->atlas_y * GLYPH_ATLAS_SIZE + e->atlas_x;
}

static uint32_t glyph_key_hash(uint32_t fh, uint32_t cp, uint16_t sx4, uint32_t phase) {
    uint32_t h = cp * 0x9E3779B1u ^ fh * 0x85EBCA77u ^ ((uint32_t)sx4 * FONT_PHASES + phase) * 0xC2B2AE3Du;
    return (h ^ (h >> 15)) & (GLYPH_HASH_SIZE - 1);
}
static void unlink_entry(struct glyph_atlas *a, uint32_t index) {
    struct glyph_cache_entry *e = &a->entries[index];
    uint32_t *link = &a->hash[glyph_key_hash(e->font_handle, e->codepoint, e->size_px_x4, e->phase)];
    while (*link && *link != index + 1) link = &a->entries[*link - 1].next;
    if (*link) *link = e->next;
    e->used = false;
    e->next = a->free_head;
    a->free_head = index + 1;
}
/* Forget everything: entries, hash, every page's packing (the pages' memory stays). */
static void atlas_reset(struct glyph_atlas *a) {
    memset(a->hash, 0, sizeof a->hash);
    for (uint32_t i = 0; i < a->n_entries; i++) a->entries[i].used = false;
    a->n_entries = 0; a->free_head = 0; a->current = 0;
    for (int p = 0; p < GLYPH_ATLAS_PAGES; p++) {
        a->pages[p].shelf_x = a->pages[p].shelf_y = a->pages[p].shelf_row_h = 0;
        a->pages[p].last_use = 0;
    }
}
/* Empty page `p`: its glyphs are forgotten and it packs from the top again. */
static void evict_page(struct glyph_atlas *a, uint32_t p) {
    for (uint32_t i = 0; i < a->n_entries; i++)
        if (a->entries[i].used && a->entries[i].page == p) unlink_entry(a, i);
    a->pages[p].shelf_x = a->pages[p].shelf_y = a->pages[p].shelf_row_h = 0;
    a->pages[p].last_use = ++a->clock;
}
static bool shelf_fits(const struct glyph_page *pg, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y) {
    uint32_t sx = pg->shelf_x, sy = pg->shelf_y;
    if (sx + w > GLYPH_ATLAS_SIZE) { sx = 0; sy += pg->shelf_row_h; }
    if (sy + h > GLYPH_ATLAS_SIZE) return false;
    *x = sx; *y = sy;
    return true;
}
/* Room for a w x h glyph: the current page, a new page, or the page used
 * longest ago, emptied. */
static bool atlas_place(struct glyph_atlas *a, uint32_t w, uint32_t h, uint32_t *page, uint32_t *x, uint32_t *y) {
    if (w > GLYPH_ATLAS_SIZE || h > GLYPH_ATLAS_SIZE) return false;
    for (int attempt = 0; attempt < 3; attempt++) {
        struct glyph_page *pg = &a->pages[a->current];
        if (!pg->coverage) {
            pg->coverage = (uint8_t *)calloc(GLYPH_ATLAS_SIZE, GLYPH_ATLAS_SIZE);
            if (!pg->coverage) return false;
        }
        if (shelf_fits(pg, w, h, x, y)) { *page = a->current; return true; }
        uint32_t next = GLYPH_ATLAS_PAGES;
        for (uint32_t p = 0; p < GLYPH_ATLAS_PAGES; p++) if (!a->pages[p].coverage) { next = p; break; }
        if (next == GLYPH_ATLAS_PAGES) {
            uint64_t oldest = UINT64_MAX;
            for (uint32_t p = 0; p < GLYPH_ATLAS_PAGES; p++)
                if (a->pages[p].last_use < oldest) { oldest = a->pages[p].last_use; next = p; }
            evict_page(a, next);
        }
        a->current = next;
    }
    return false;
}
static struct glyph_cache_entry *new_entry(struct glyph_atlas *a) {
    uint32_t index;
    if (a->free_head) { index = a->free_head - 1; a->free_head = a->entries[index].next; }
    else if (a->n_entries < GLYPH_CACHE_MAX) index = a->n_entries++;
    else {
        /* every entry in use: give back the oldest page's, or all if they
         * were blank glyphs (no page to give back) */
        uint64_t oldest = UINT64_MAX; uint32_t victim = 0;
        for (uint32_t p = 0; p < GLYPH_ATLAS_PAGES; p++)
            if (a->pages[p].coverage && a->pages[p].last_use < oldest) { oldest = a->pages[p].last_use; victim = p; }
        evict_page(a, victim);
        if (!a->free_head) atlas_reset(a);
        return new_entry(a);
    }
    struct glyph_cache_entry *e = &a->entries[index];
    memset(e, 0, sizeof *e);
    return e;
}

struct glyph_cache_entry *glyph_cache_lookup_or_rasterize(
    struct glyph_atlas *atlas, struct font *f, uint32_t codepoint, float size_px) {
    return glyph_cache_lookup_phase(atlas, f, codepoint, size_px, 0);
}

struct glyph_cache_entry *glyph_cache_lookup_phase(
    struct glyph_atlas *atlas, struct font *f, uint32_t codepoint, float size_px, uint32_t phase) {
    if (!atlas || !f) return 0;
    if (phase >= FONT_PHASES) phase = 0;
    uint16_t sx4 = (uint16_t)(size_px * 4.0f + 0.5f);
    uint32_t fh = (uint32_t)(f - g_fonts) + 1;      /* the cache key's font */
    if (f < g_fonts || fh > FONT_MAX) return 0;
    uint32_t bucket = glyph_key_hash(fh, codepoint, sx4, phase);
    for (uint32_t i = atlas->hash[bucket]; i; i = atlas->entries[i - 1].next) {
        struct glyph_cache_entry *e = &atlas->entries[i - 1];
        if (e->font_handle == fh && e->codepoint == codepoint && e->size_px_x4 == sx4 && e->phase == phase) {
            if (e->page < GLYPH_ATLAS_PAGES) atlas->pages[e->page].last_use = ++atlas->clock;
            return e;   /* cache hit -- no rasterize */
        }
    }

    /* miss: rasterize */
    uint32_t gi = font_codepoint_to_glyph(f, codepoint);   /* 0 == .notdef, still rendered */
    uint8_t *cov = 0; int w = 0, h = 0, bx = 0, by = 0; float adv = 0;
    if (!font_rasterize_glyph_at(f,gi,(float)sx4/4.0f,(float)phase/FONT_PHASES,&cov,&w,&h,&bx,&by,&adv)) return 0;

    /* The entry first: making room for it may empty a page, which must not be
     * the page this glyph is about to be packed into. */
    struct glyph_cache_entry *e = new_entry(atlas);
    uint32_t page = GLYPH_NO_PAGE, ax = 0, ay = 0;
    if (cov && w > 0 && h > 0 && !atlas_place(atlas, (uint32_t)w, (uint32_t)h, &page, &ax, &ay)) {
        free(cov); cov = 0; w = h = 0;             /* bigger than a page: drawn as nothing */
    }
    e->used = true; e->font_handle = fh; e->codepoint = codepoint; e->size_px_x4 = sx4; e->phase = (uint8_t)phase;
    e->bearing_x = (int16_t)bx; e->bearing_y = (int16_t)by; e->advance_px = adv;
    e->missing = gi == 0;
    e->atlas_w = (uint16_t)(w > 0 ? w : 0); e->atlas_h = (uint16_t)(h > 0 ? h : 0);
    e->page = (uint8_t)page;
    if (cov && w > 0 && h > 0) {
        struct glyph_page *pg = &atlas->pages[page];
        if (ax == 0 && ay != pg->shelf_y) { pg->shelf_y = ay; pg->shelf_row_h = 0; }   /* a new row */
        e->atlas_x = (uint16_t)ax; e->atlas_y = (uint16_t)ay;
        for (int yy = 0; yy < h; yy++)
            memcpy(pg->coverage + (size_t)(ay + (uint32_t)yy) * GLYPH_ATLAS_SIZE + ax, cov + (size_t)yy * w, (size_t)w);
        pg->shelf_x = ax + (uint32_t)w;
        if ((uint32_t)h > pg->shelf_row_h) pg->shelf_row_h = (uint32_t)h;
        pg->last_use = ++atlas->clock;
    } else {
        e->atlas_x = e->atlas_y = 0;   /* blank glyph (space): no pixels, real advance */
    }
    free(cov);
    e->next = atlas->hash[bucket];
    atlas->hash[bucket] = (uint32_t)(e - atlas->entries) + 1;
    return e;
}

/* ------------------------------------------------------------------------- */
/* Section 6: draw_text                                                       */
/* ------------------------------------------------------------------------- */

/* Standard 1-4 byte UTF-8 decode; returns bytes consumed (>=1).
 * EXPORTED (font_utf8_decode) so layout measures a string exactly the way this
 * file draws it. They used to disagree -- see the note in layout.c. */
int font_utf8_decode(const char *s, uint32_t *cp) {
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12)
            | ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4;
    }
    *cp = 0xFFFD; return 1;   /* invalid byte -> replacement, advance one */
}

/* Gamma (approx 2.0) blend tables, built once. The glyph blend mixes in LINEAR
 * light: out = sqrt(src^2*eff + dst^2*inv). Done literally that was 3 dst
 * squarings + 3 sqrtf PER TEXT PIXEL -- and in the freestanding build sqrtf is
 * an 8-iteration Newton loop (24 float divides/pixel just to de-gamma). Text
 * covers most of the screen, so this dominated glyph cost under TCG. Instead:
 *   G_LIN[d]   = (d/255)^2                    -- linearise a dst byte (no square)
 *   G_DELIN[k] = round(sqrt(k/4096)*255)      -- de-linearise (no sqrt)
 * The blend arg is a convex mix of two [0,1] values so it stays in [0,1]; the
 * 12-bit de-gamma table quantises to <=1/255, visually identical. */
#define G_DELIN_BITS 12
#define G_DELIN_N    (1 << G_DELIN_BITS)   /* 4096 */
static float   g_lin[256];
static uint8_t g_delin[G_DELIN_N + 1];
static int     g_gamma_ready;
static void gamma_tables_init(void) {
    if (g_gamma_ready) return;
    for (int d = 0; d < 256; d++) { float x = d / 255.0f; g_lin[d] = x * x; }
    for (int k = 0; k <= G_DELIN_N; k++) {
        int v = (int)(sqrtf((float)k / (float)G_DELIN_N) * 255.0f + 0.5f);
        g_delin[k] = (uint8_t)(v > 255 ? 255 : v);
    }
    g_gamma_ready = 1;
}

/* premultiplied-BGRA pixel over-blend, gated by the backend clip/dirty. */
static void blit_coverage_tinted(struct render_target *rt, int dx, int dy,
                                 const uint8_t *coverage, int aw, int ah,
                                 struct color color, float opacity,
                                 float ox, float oy, float box_w, float box_h,
                                 const struct paint *paint) {
    if (!g_gamma_ready) gamma_tables_init();
    bool grad = paint && paint->kind != PAINT_NONE;
    /* src linear-light channels: constant for a flat glyph, resampled per pixel
     * for a gradient (coords are text-box-local so the ramp spans the whole run). */
    float sr2 = color.r * color.r, sg2 = color.g * color.g, sb2 = color.b * color.b;
    float sa = color.a;
    /* A glyph is a small box, and the clip/dirty coverage over it is almost
     * always uniformly 1 -- so ask ONCE for the whole cell instead of once per
     * pixel. Text is the bulk of a document's pixels, and cpu_coverage_at walks
     * the clip stack and the damage list every time it is called. */
    int cov_all = cpu_box_fully_covered(dx, dy, dx + aw, dy + ah);
    for (int yy = 0; yy < ah; yy++) {
        int py = dy + yy;
        if (py < 0 || py >= (int)rt->height) continue;
        uint32_t *row = (uint32_t *)((unsigned char *)rt->pixels + (size_t)py * rt->stride);
        for (int xx = 0; xx < aw; xx++) {
            int px = dx + xx;
            if (px < 0 || px >= (int)rt->width) continue;
            float cov = coverage[(size_t)yy * GLYPH_ATLAS_SIZE + xx] / 255.0f;
            if (grad) {
                struct color gc = cpu_paint_at(paint, (float)px - ox, (float)py - oy, box_w, box_h);
                sr2 = gc.r * gc.r; sg2 = gc.g * gc.g; sb2 = gc.b * gc.b; sa = gc.a;
            }
            float eff = sa * cov * opacity;
            if (!cov_all) eff *= cpu_coverage_at(px + 0.5f, py + 0.5f);
            if (eff <= 0.0f) continue;
            uint32_t d = row[px];
            int db = d & 255, dg = (d>>8)&255, dr = (d>>16)&255, da = (d>>24)&255;
            float inv = 1.0f - eff;
            /* linear-light blend via tables: linearise dst (g_lin), mix, then
             * de-linearise (g_delin) -- no per-pixel square or sqrt. */
            float ar = sr2*eff + g_lin[dr]*inv;
            float ag = sg2*eff + g_lin[dg]*inv;
            float ab = sb2*eff + g_lin[db]*inv;
            int kr = (int)(ar * (float)G_DELIN_N); if (kr > G_DELIN_N) kr = G_DELIN_N; if (kr < 0) kr = 0;
            int kg = (int)(ag * (float)G_DELIN_N); if (kg > G_DELIN_N) kg = G_DELIN_N; if (kg < 0) kg = 0;
            int kb = (int)(ab * (float)G_DELIN_N); if (kb > G_DELIN_N) kb = G_DELIN_N; if (kb < 0) kb = 0;
            int nr = g_delin[kr], ng = g_delin[kg], nb = g_delin[kb];
            int na = (int)(eff * 255.0f + da * inv + 0.5f);
            if (na>255) na=255;
            row[px] = ((uint32_t)na<<24)|((uint32_t)nr<<16)|((uint32_t)ng<<8)|(uint32_t)nb;
        }
    }
}

/* One allocation-free line walk for measurement and painting. Spaces retain
 * their advances; a wrap drops the separator before the next word. Width zero
 * preserves preformatted rows. Explicit line breaks are always honored. */
static float layout_advance(struct font *f,uint32_t cp,float size) {
    float canonical=(float)(uint16_t)(size*4.0f+.5f)/4.0f;
    f=font_for_codepoint(f,cp);
    return glyph_advance(f,font_codepoint_to_glyph(f,cp))*canonical/f->units_per_em;
}
int font_layout_text(struct font *f,const char *text,float size,float width,
                     font_text_visitor visit,void *context) {
    if(!f || !text || !f->units_per_em)return 1;
    if(!visit && width<=0) {
        int lines=1;
        for(int i=0;text[i] && i<8192;i++) {
            if(text[i]=='\n')lines++;
            else if(text[i]=='\r'){lines++;if(text[i+1]=='\n')i++;}
        }
        return lines;
    }
    float line_height=(f->ascent+f->descent+f->line_gap)*size/f->units_per_em;
    float x=0;int line=0,scanned=0;const char *p=text;
    uint32_t prev=0;                       /* the glyph before: kerned against it */
    while(*p && scanned<8192) {
        if(*p=='\n' || *p=='\r') {
            char ch=*p++;scanned++;
            if(ch=='\r' && *p=='\n'){p++;scanned++;}
            line++;x=0;prev=0;continue;
        }
        if(*p==' ' || *p=='\t') {
            x+=layout_advance(f,' ',size)*(*p=='\t'?4:1);p++;scanned++;prev=0;continue;
        }
        const char *end=p;float word=0;int bytes=0;uint32_t wp=prev;
        while(*end && *end!=' ' && *end!='\t' && *end!='\n' && *end!='\r' && scanned+bytes<8192) {
            uint32_t cp;int n=font_utf8_decode(end,&cp);end+=n;bytes+=n;
            word+=font_pair_px(f,wp,cp,size)+layout_advance(f,cp,size);wp=cp;
        }
        if(width>0 && x>0 && x+word>width+.01f){line++;x=0;}
        if(!visit && (width<=0 || word<=width)) {
            x+=word;p=end;scanned+=bytes;prev=wp;continue;
        }
        while(p<end) {
            uint32_t cp;int n=font_utf8_decode(p,&cp);p+=n;scanned+=n;
            /* the pair's kerning before the fit test: a box measured to the
             * kerned width must hold its last glyph, not wrap it away */
            float advance=layout_advance(f,cp,size),kern=font_pair_px(f,prev,cp,size);
            if(width>0 && x>0 && x+kern+advance>width+.01f){line++;x=0;prev=0;kern=0;}
            x+=kern;
            if(visit)visit(context,cp,x,line*line_height);
            x+=advance;prev=cp;
        }
    }
    return line+1;
}

struct text_draw_context {
    struct render_target *rt;struct font *font;float x,y,size,ascent,opacity,width,height;
    struct color color;const struct paint *paint;
};
static void draw_positioned_glyph(void *opaque,uint32_t cp,float x,float y) {
    struct text_draw_context *c=opaque;
    if(c->height>0 && y>=c->height)return;
    /* the pen to a quarter pixel (FONT_PHASES): its whole pixel, and the
     * glyph rasterised at the fraction */
    float pen=c->x+x;int ip=(int)floorf_(pen);
    int phase=(int)((pen-(float)ip)*FONT_PHASES+.5f);
    if(phase>=FONT_PHASES){ip++;phase=0;}
    struct glyph_cache_entry *g=glyph_cache_lookup_phase(&g_atlas,font_for_codepoint(c->font,cp),cp,c->size,(uint32_t)phase);
    const uint8_t *coverage=glyph_coverage(&g_atlas,g);
    if(coverage && g->atlas_h>0)
        blit_coverage_tinted(c->rt,ip+g->bearing_x,(int)(c->y+y+c->ascent-g->bearing_y),
            coverage,g->atlas_w,g->atlas_h,c->color,c->opacity,
            c->x,c->y,c->width,c->height,c->paint);
}
static void backend_draw_text_impl(struct render_target *rt,float x,float y,const char *utf8,
                                   uint32_t handle,float size,struct color color,float opacity,
                                   const struct paint *paint,float width,float height,bool wrap) {
    struct font *f=font_for_handle(handle);if(!f || !utf8)return;
    struct text_draw_context c={.rt=rt,.font=f,.x=x,.y=y,.size=size,
        .ascent=f->ascent*size/f->units_per_em,.opacity=opacity,.width=width,.height=height,
        .color=color,.paint=paint};
    font_layout_text(f,utf8,size,wrap?width:0,draw_positioned_glyph,&c);
}

/* THE ONE LINE that ties this file to the toolkit. Everything above it is a
 * TrueType parser, a rasteriser and a glyph cache that need nothing but malloc
 * and memcpy -- which is why the NetSurf port can compile this very file into
 * a browser that has no EmUI in it at all, and get the OS's own typeface.
 * FONT_NO_BACKEND is that seam, and it is here rather than in the port so the
 * dependency is documented where it exists. */
#ifndef FONT_NO_BACKEND
void font_install_backend(void) {
    cpu_backend_get()->draw_text = backend_draw_text_impl;
}
#else
void font_install_backend(void) { }
#endif
