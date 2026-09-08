/* user/web/jpeg.c -- baseline JPEG. See jpeg.h.
 *
 * Four algorithms, in the order the bytes demand them:
 *
 *   1. MARKERS      a segment stream: quantisation tables, Huffman tables, the
 *                   frame header, then one scan of entropy-coded data.
 *   2. HUFFMAN      canonical codes, rebuilt from a table of code LENGTHS. The
 *                   bit reader must also un-stuff 0xFF00 -- a literal 0xFF in
 *                   the data is written as two bytes so it cannot be mistaken
 *                   for a marker, and forgetting that decodes garbage a few
 *                   hundred bytes in, long after the point of failure.
 *   3. IDCT         each 8x8 block is coefficients in frequency space;
 *                   inverting the transform is what turns them into pixels.
 *   4. COLOUR       YCbCr, with the chroma planes usually at half resolution.
 *                   Upsampling and the conversion are the last pass.
 *
 * The zig-zag is the thread through all of it: coefficients are STORED in a
 * diagonal order (so the mostly-zero high frequencies bunch at the end and
 * run-length coding pays off) and must be scattered back to a raster 8x8
 * before the transform. Getting that one table backwards produces an image
 * that is recognisable but subtly wrong, which is the hardest kind to debug --
 * so it is applied in exactly one place, at dequantisation.
 */
#include <string.h>

#include "jpeg.h"

#define MAX_COMPS 3          /* greyscale or YCbCr; CMYK is refused */

struct huff {
    uint8_t  bits[17];       /* count of codes per length 1..16 */
    uint8_t  vals[256];
    int      mincode[17], maxcode[18], valptr[17];
    int      present;
};

struct comp {
    int id, h, v, tq;        /* sampling factors, quant table    */
    int td, ta;              /* DC / AC Huffman tables           */
    int dcpred;
    uint8_t *plane;          /* w_blocks*8 x h_blocks*8          */
    int pw, ph;
    int hsh, vsh;            /* log2 of the subsampling ratio     */
};

struct jdec {
    const uint8_t *p, *end;
    uint32_t bitbuf;
    int      bitcnt;
    uint16_t qt[4][64];
    struct huff hdc[4], hac[4];
    struct comp comp[MAX_COMPS];
    int ncomp, w, h, hmax, vmax, mcux, mcuy, restart;
};

static const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63,
};

/* ---- bit reader ---------------------------------------------------------- */

/* Refill one byte, un-stuffing 0xFF00 and stopping at any real marker. */
static int fill_byte(struct jdec *j) {
    if (j->p >= j->end) return -1;
    uint8_t b = *j->p++;
    if (b == 0xFF) {
        if (j->p >= j->end) return -1;
        uint8_t n = *j->p;
        if (n == 0x00) { j->p++; }          /* stuffed literal 0xFF */
        else return -1;                      /* a marker: the scan ends here */
    }
    return b;
}

static int get_bits(struct jdec *j, int n) {
    while (j->bitcnt < n) {
        int b = fill_byte(j);
        /* Past the end of the scan, feed ZEROES rather than failing. A
         * truncated JPEG should show the part that arrived -- which is what
         * every browser does, and what makes a slow image appear top-down
         * rather than not at all. */
        if (b < 0) b = 0;
        j->bitbuf = (j->bitbuf << 8) | (uint32_t)b;
        j->bitcnt += 8;
    }
    j->bitcnt -= n;
    return (int)((j->bitbuf >> j->bitcnt) & ((1u << n) - 1u));
}

/* A JPEG coefficient is stored as a magnitude category plus that many bits;
 * the top half of the range is positive and the bottom half is negative. */
static int extend(int v, int n) {
    return (n && v < (1 << (n - 1))) ? v - (1 << n) + 1 : v;
}

static int huff_decode(struct jdec *j, struct huff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | get_bits(j, 1);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l])
            return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return 0;                                /* corrupt: treat as EOB */
}

static void huff_build(struct huff *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

/* ---- IDCT ----------------------------------------------------------------
 *
 * FIXED POINT, not float, and that is a portability decision rather than a
 * micro-optimisation. This OS runs under TCG, where SSE scalar float is
 * emulated in software while integer arithmetic maps straight to the host --
 * so a float inner loop is not "a bit slower", it is a different order of
 * magnitude. The float version of this file decoded a 320x180 photo in 2.7ms
 * natively and TWENTY-ONE SECONDS on the metal; that is the gap being closed.
 *
 * Scale is 2^10 per pass. Dequantised coefficients reach ~32k, so
 * 32768 * 1024 * 8 stays inside int32 with room -- the reason the scale is 10
 * and not the 13 a native decoder would use.
 */
#define IDCT_BITS  10
#define IDCT_ONE   (1 << IDCT_BITS)

static int32_t g_cos[8][8];      /* [x][u], including the orthonormal alpha */
static int     g_cos_init;

static void idct_init(void) {
    if (g_cos_init) return;
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++) {
            double a = (2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0;
            double t = a;
            while (t >  3.14159265358979323846) t -= 2.0 * 3.14159265358979323846;
            while (t < -3.14159265358979323846) t += 2.0 * 3.14159265358979323846;
            double t2 = t * t, cs = 1.0, term = 1.0;
            for (int n = 1; n <= 12; n++) {
                term *= -t2 / ((2.0 * n - 1.0) * (2.0 * n));
                cs += term;
            }
            /* sqrt(1/8) for u==0, sqrt(2/8) otherwise: the orthonormal DCT-III
             * per dimension, so applying it twice is the full 2D transform */
            double alpha = (u == 0) ? 0.35355339059327376 : 0.5;
            g_cos[x][u] = (int32_t)(cs * alpha * IDCT_ONE + (cs * alpha >= 0 ? 0.5 : -0.5));
            g_cos_init = 1;
        }
}

static void idct8(int32_t *b) {
    int32_t tmp[64];
    for (int y = 0; y < 8; y++) {                     /* rows */
        const int32_t *r = b + y * 8;
        /* A row of only a DC term is extremely common in a photograph, and
         * its transform is a constant -- skipping the eight sums here is most
         * of what makes a smooth image cheap. */
        if (!r[1] && !r[2] && !r[3] && !r[4] && !r[5] && !r[6] && !r[7]) {
            int32_t v = (r[0] * g_cos[0][0] + (IDCT_ONE >> 1)) >> IDCT_BITS;
            for (int x = 0; x < 8; x++) tmp[y * 8 + x] = v;
            continue;
        }
        for (int x = 0; x < 8; x++) {
            int32_t s = 0;
            for (int u = 0; u < 8; u++) s += g_cos[x][u] * r[u];
            tmp[y * 8 + x] = (s + (IDCT_ONE >> 1)) >> IDCT_BITS;
        }
    }
    for (int x = 0; x < 8; x++) {                     /* columns */
        for (int y = 0; y < 8; y++) {
            int32_t s = 0;
            for (int v = 0; v < 8; v++) s += g_cos[y][v] * tmp[v * 8 + x];
            b[y * 8 + x] = (s + (IDCT_ONE >> 1)) >> IDCT_BITS;
        }
    }
}

static uint8_t clamp8(int32_t v) {
    int i = (int)v + 128;
    return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

/* Decode one 8x8 block straight into its component's plane. */
static int decode_block(struct jdec *j, struct comp *c, int bx, int by) {
    int32_t blk[64];
    memset(blk, 0, sizeof blk);
    int any_ac = 0;

    int t = huff_decode(j, &j->hdc[c->td]);
    int diff = t ? extend(get_bits(j, t), t) : 0;
    c->dcpred += diff;
    /* The zig-zag is applied HERE and nowhere else -- see the file header. */
    blk[0] = c->dcpred * (int32_t)j->qt[c->tq][0];

    for (int k = 1; k < 64; ) {
        int rs = huff_decode(j, &j->hac[c->ta]);
        int r = rs >> 4, sz = rs & 15;
        if (!sz) {
            if (r != 15) break;               /* EOB */
            k += 16;                           /* ZRL: sixteen zeroes */
            continue;
        }
        k += r;
        if (k > 63) break;
        int v = extend(get_bits(j, sz), sz);
        blk[ZIGZAG[k]] = v * (int32_t)j->qt[c->tq][k];
        any_ac = 1;
        k++;
    }

    /* THE flat-block shortcut. In a photograph most 8x8 blocks carry only a
     * DC term, and their inverse transform is a single constant -- so the
     * whole 128-multiply pass collapses to a memset. This is the difference
     * between a decoder that works and one you can browse with. */
    if (!any_ac) {
        /* both passes contribute g_cos[0][0], so the constant is
         * DC * c00^2 -- i.e. DC/8 for an orthonormal transform */
        int32_t t0 = (blk[0] * g_cos[0][0] + (IDCT_ONE >> 1)) >> IDCT_BITS;
        uint8_t v = clamp8((t0 * g_cos[0][0] + (IDCT_ONE >> 1)) >> IDCT_BITS);
        for (int y = 0; y < 8; y++) {
            int py = by * 8 + y;
            if (py >= c->ph) break;
            uint8_t *row = c->plane + (size_t)py * c->pw;
            for (int x = 0; x < 8; x++) {
                int px = bx * 8 + x;
                if (px >= c->pw) break;
                row[px] = v;
            }
        }
        return 0;
    }

    idct8(blk);

    for (int y = 0; y < 8; y++) {
        int py = by * 8 + y;
        if (py >= c->ph) break;
        uint8_t *row = c->plane + (size_t)py * c->pw;
        for (int x = 0; x < 8; x++) {
            int px = bx * 8 + x;
            if (px >= c->pw) break;
            row[px] = clamp8(blk[y * 8 + x]);
        }
    }
    return 0;
}

/* ---- markers ------------------------------------------------------------- */

static int rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

/* Walk the segments up to (and including) SOS. `hdr_only` stops at SOF0. */
static int parse_headers(struct jdec *j, const uint8_t *src, size_t len, int hdr_only) {
    if (len < 4 || src[0] != 0xFF || src[1] != 0xD8) return JPG_ENOTJPG;
    size_t i = 2;
    while (i + 4 <= len) {
        if (src[i] != 0xFF) { i++; continue; }
        uint8_t m = src[i + 1];
        i += 2;
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD9) break;
        if (i + 2 > len) return JPG_EDATA;
        int seg = rd16(src + i);
        if (seg < 2 || i + (size_t)seg > len) return JPG_EDATA;
        const uint8_t *d = src + i + 2;
        int dl = seg - 2;

        switch (m) {
        case 0xC0: case 0xC1: {                        /* SOF0/SOF1 baseline */
            if (dl < 6) return JPG_EDATA;
            j->h = rd16(d + 1);
            j->w = rd16(d + 3);
            j->ncomp = d[5];
            if (j->ncomp != 1 && j->ncomp != 3) return JPG_EUNSUP;
            if (dl < 6 + j->ncomp * 3) return JPG_EDATA;
            for (int k = 0; k < j->ncomp; k++) {
                const uint8_t *e = d + 6 + k * 3;
                j->comp[k].id = e[0];
                j->comp[k].h  = e[1] >> 4;
                j->comp[k].v  = e[1] & 15;
                j->comp[k].tq = e[2] & 3;
                if (j->comp[k].h < 1 || j->comp[k].h > 2 ||
                    j->comp[k].v < 1 || j->comp[k].v > 2) return JPG_EUNSUP;
            }
            if (hdr_only) return JPG_OK;
            break;
        }
        /* Progressive (C2), arithmetic (C9-CB), lossless, hierarchical: a
         * different decoder. Refused, not approximated. */
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            return JPG_EUNSUP;
        case 0xC4: {                                    /* DHT */
            int o = 0;
            while (o + 17 <= dl) {
                int tc = d[o] >> 4, th = d[o] & 3;
                struct huff *h = tc ? &j->hac[th] : &j->hdc[th];
                memset(h, 0, sizeof *h);
                int total = 0;
                for (int l = 1; l <= 16; l++) { h->bits[l] = d[o + l]; total += h->bits[l]; }
                o += 17;
                if (total > 256 || o + total > dl) return JPG_EDATA;
                for (int k = 0; k < total; k++) h->vals[k] = d[o + k];
                o += total;
                huff_build(h);
                h->present = 1;
            }
            break;
        }
        case 0xDB: {                                    /* DQT */
            int o = 0;
            while (o < dl) {
                int pq = d[o] >> 4, tq = d[o] & 3;
                o++;
                for (int k = 0; k < 64; k++) {
                    if (pq) { if (o + 1 >= dl) return JPG_EDATA;
                              j->qt[tq][k] = (uint16_t)rd16(d + o); o += 2; }
                    else    { if (o >= dl) return JPG_EDATA;
                              j->qt[tq][k] = d[o]; o += 1; }
                }
            }
            break;
        }
        case 0xDD:                                      /* DRI */
            if (dl >= 2) j->restart = rd16(d);
            break;
        case 0xDA: {                                    /* SOS */
            if (dl < 1) return JPG_EDATA;
            int ns = d[0];
            if (ns != j->ncomp) return JPG_EUNSUP;      /* non-interleaved scan */
            for (int k = 0; k < ns; k++) {
                int cid = d[1 + k * 2], tt = d[2 + k * 2];
                for (int c = 0; c < j->ncomp; c++)
                    if (j->comp[c].id == cid) {
                        j->comp[c].td = tt >> 4;
                        j->comp[c].ta = tt & 15;
                    }
            }
            j->p = src + i + seg;
            j->end = src + len;
            return JPG_OK;
        }
        default: break;                                 /* APPn, COM: skipped */
        }
        i += (size_t)seg;
    }
    return hdr_only ? JPG_ENOTJPG : JPG_EDATA;
}

int jpeg_probe(const uint8_t *src, size_t len, uint32_t *w, uint32_t *h) {
    static struct jdec j;
    memset(&j, 0, sizeof j);
    int rc = parse_headers(&j, src, len, 1);
    if (rc != JPG_OK) return rc;
    if (!j.w || !j.h) return JPG_ENOTJPG;
    if (w) *w = (uint32_t)j.w;
    if (h) *h = (uint32_t)j.h;
    return JPG_OK;
}

/* ---- the scan ------------------------------------------------------------ */

int jpeg_decode(const uint8_t *src, size_t len,
                uint32_t *dst, size_t dst_cap,
                uint8_t *scratch, size_t scratch_cap,
                uint32_t *out_w, uint32_t *out_h) {
    static struct jdec j;
    memset(&j, 0, sizeof j);

    int rc = parse_headers(&j, src, len, 0);
    if (rc != JPG_OK) return rc;
    if (!j.w || !j.h) return JPG_ENOTJPG;
    idct_init();                     /* the cosine table, built once */
    if ((size_t)j.w * (size_t)j.h > dst_cap / 4) return JPG_ETOOBIG;

    j.hmax = j.vmax = 1;
    for (int c = 0; c < j.ncomp; c++) {
        if (j.comp[c].h > j.hmax) j.hmax = j.comp[c].h;
        if (j.comp[c].v > j.vmax) j.vmax = j.comp[c].v;
    }
    j.mcux = (j.w + 8 * j.hmax - 1) / (8 * j.hmax);
    j.mcuy = (j.h + 8 * j.vmax - 1) / (8 * j.vmax);

    /* Each component gets a plane sized to WHOLE MCUs: a JPEG's blocks run
     * past the stated edges, and decoding into an exactly-sized plane would
     * write out of bounds on the last row and column of most images. */
    size_t used = 0;
    for (int c = 0; c < j.ncomp; c++) {
        /* hmax/h is 1 or 2 for every baseline file (factors are 1 or 2) */
        j.comp[c].hsh = (j.hmax / j.comp[c].h) == 2 ? 1 : 0;
        j.comp[c].vsh = (j.vmax / j.comp[c].v) == 2 ? 1 : 0;
        j.comp[c].pw = j.mcux * 8 * j.comp[c].h;
        j.comp[c].ph = j.mcuy * 8 * j.comp[c].v;
        size_t need = (size_t)j.comp[c].pw * j.comp[c].ph;
        if (used + need > scratch_cap) return JPG_ETOOBIG;
        j.comp[c].plane = scratch + used;
        memset(j.comp[c].plane, 128, need);
        used += need;
    }

    for (int c = 0; c < j.ncomp; c++)
        if (!j.hdc[j.comp[c].td].present || !j.hac[j.comp[c].ta].present)
            return JPG_EDATA;

    int mcu = 0;
    for (int my = 0; my < j.mcuy; my++) {
        for (int mx = 0; mx < j.mcux; mx++) {
            /* A restart interval resets the bit stream and every DC predictor:
             * that is the whole point of restarts, so a corrupt stretch cannot
             * smear the rest of the picture. */
            if (j.restart && mcu && mcu % j.restart == 0) {
                j.bitcnt = 0; j.bitbuf = 0;
                while (j.p + 1 < j.end && !(j.p[0] == 0xFF &&
                       j.p[1] >= 0xD0 && j.p[1] <= 0xD7)) j.p++;
                if (j.p + 1 < j.end) j.p += 2;
                for (int c = 0; c < j.ncomp; c++) j.comp[c].dcpred = 0;
            }
            for (int c = 0; c < j.ncomp; c++)
                for (int by = 0; by < j.comp[c].v; by++)
                    for (int bx = 0; bx < j.comp[c].h; bx++)
                        decode_block(&j, &j.comp[c],
                                     mx * j.comp[c].h + bx,
                                     my * j.comp[c].v + by);
            mcu++;
        }
    }

    /* ---- upsample + YCbCr -> BGRA premultiplied (opaque) ---- */
    for (int y = 0; y < j.h; y++) {
        uint32_t *o = dst + (size_t)y * j.w;
        for (int x = 0; x < j.w; x++) {
            /* h/hmax is 1 or 1/2 in every baseline JPEG, so the sample index
             * is a SHIFT, not a divide -- three integer divisions per pixel is
             * a quarter of a million on a small photo, and TCG charges for
             * every one. */
            int Y, Cb = 128, Cr = 128;
            {
                struct comp *c = &j.comp[0];
                Y = c->plane[(size_t)(y >> c->vsh) * c->pw + (x >> c->hsh)];
            }
            if (j.ncomp == 3) {
                struct comp *c1 = &j.comp[1], *c2 = &j.comp[2];
                Cb = c1->plane[(size_t)(y >> c1->vsh) * c1->pw + (x >> c1->hsh)];
                Cr = c2->plane[(size_t)(y >> c2->vsh) * c2->pw + (x >> c2->hsh)];
            }
            int r = Y + ((91881 * (Cr - 128)) >> 16);
            int g = Y - ((22554 * (Cb - 128) + 46802 * (Cr - 128)) >> 16);
            int b = Y + ((116130 * (Cb - 128)) >> 16);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            /* JPEG has no alpha, so premultiplied and straight agree */
            o[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    if (out_w) *out_w = (uint32_t)j.w;
    if (out_h) *out_h = (uint32_t)j.h;
    return JPG_OK;
}
