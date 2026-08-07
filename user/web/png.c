/* user/web/png.c -- see png.h.
 *
 * The three parts of a PNG, in the order they resist you:
 *
 *   1. CHUNKS      length-tag-data-crc, repeated. Easy, but IDAT may be split
 *                  across many chunks and must be CONCATENATED before it is a
 *                  valid stream -- a decoder that inflates each one separately
 *                  works on small files and fails on large ones, which is the
 *                  worst way for a bug to behave.
 *   2. ZLIB        PNG wraps DEFLATE in RFC 1950: two header bytes and an
 *                  adler32 trailer. We already have the DEFLATE half.
 *   3. UNFILTER    each scanline is prefixed by a filter byte and reconstructed
 *                  from its left/above neighbours. This is the part that is
 *                  genuinely PNG's own, and it must run in place, in order.
 */
#include <string.h>

#include "inflate.h"
#include "png.h"

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static const uint8_t PNG_SIG[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };

/* channels per pixel for a PNG colour type */
static int chans_for(int ct) {
    switch (ct) {
        case 0: return 1;   /* grey        */
        case 2: return 3;   /* RGB         */
        case 3: return 1;   /* palette idx */
        case 4: return 2;   /* grey+alpha  */
        case 6: return 4;   /* RGBA        */
    }
    return 0;
}

struct hdr { uint32_t w, h; int depth, ctype, interlace; };

static int read_hdr(const uint8_t *src, size_t len, struct hdr *h) {
    if (len < 8 + 25 || memcmp(src, PNG_SIG, 8) != 0) return PNG_ENOTPNG;
    if (be32(src + 8) != 13 || memcmp(src + 12, "IHDR", 4) != 0) return PNG_ENOTPNG;
    h->w = be32(src + 16);
    h->h = be32(src + 20);
    h->depth     = src[24];
    h->ctype     = src[25];
    h->interlace = src[28];
    if (!h->w || !h->h) return PNG_ENOTPNG;
    if (!chans_for(h->ctype)) return PNG_ENOTPNG;
    return PNG_OK;
}

int png_probe(const uint8_t *src, size_t len, uint32_t *w, uint32_t *hh) {
    struct hdr h;
    int rc = read_hdr(src, len, &h);
    if (rc != PNG_OK) return rc;
    if (w)  *w  = h.w;
    if (hh) *hh = h.h;
    return PNG_OK;
}

static int paeth(int a, int b, int c) {          /* a=left b=up c=up-left */
    int p = a + b - c, pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* One scanline, reconstructed in place from its filter byte and the line
 * above. `bpp` is bytes per pixel ROUNDED UP -- for sub-byte depths PNG
 * filters on bytes, not pixels, and bpp is 1. */
static void unfilter(uint8_t *cur, const uint8_t *up, size_t n, int bpp, int f) {
    switch (f) {
        case 0: break;                                        /* None */
        case 1: for (size_t i = (size_t)bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + cur[i-bpp]); break;
        case 2: if (up) for (size_t i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + up[i]); break;
        case 3:
            for (size_t i = 0; i < n; i++) {
                int a = i >= (size_t)bpp ? cur[i-bpp] : 0, b = up ? up[i] : 0;
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < n; i++) {
                int a = i >= (size_t)bpp ? cur[i-bpp] : 0;
                int b = up ? up[i] : 0;
                int c = (up && i >= (size_t)bpp) ? up[i-bpp] : 0;
                cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
            }
            break;
        default: break;                                       /* treated as None */
    }
}

/* premultiplied BGRA, which is what the scene blits without conversion */
static uint32_t bgra(int r, int g, int b, int a) {
    if (a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int png_decode(const uint8_t *src, size_t len,
               uint32_t *dst, size_t dst_cap,
               uint8_t *scratch, size_t scratch_cap,
               uint32_t *out_w, uint32_t *out_h)
{
    struct hdr h;
    int rc = read_hdr(src, len, &h);
    if (rc != PNG_OK) return rc;

    /* Adam7 is a different decoder wearing the same file extension: seven
     * sub-images with their own dimensions and filters. Refusing is honest;
     * half-decoding it would render garbage that looks like a bug elsewhere. */
    if (h.interlace) return PNG_EUNSUP;
    if (h.depth != 1 && h.depth != 2 && h.depth != 4 && h.depth != 8 && h.depth != 16)
        return PNG_EUNSUP;
    if (h.ctype != 3 && h.depth < 8) return PNG_EUNSUP;   /* sub-byte grey: rare */

    if ((size_t)h.w * h.h > dst_cap / 4) return PNG_ETOOBIG;

    int chans = chans_for(h.ctype);
    int bits  = h.depth * chans;
    size_t stride = ((size_t)h.w * (size_t)bits + 7) / 8;      /* bytes per row */
    size_t need   = (stride + 1) * (size_t)h.h;
    if (need > scratch_cap) return PNG_ETOOBIG;

    /* --- pass 1: gather PLTE/tRNS and CONCATENATE every IDAT ------------- */
    uint8_t pal[256][3];   int have_pal = 0;
    uint8_t palא[256];     /* per-index alpha from tRNS; 255 by default */
    for (int i = 0; i < 256; i++) palא[i] = 255;
    int trns_grey = -1, trns_r = -1, trns_g = -1, trns_b = -1;

    /* the compressed stream is assembled at the END of scratch, so the
     * inflated result can use the front without overlapping it */
    size_t zcap = scratch_cap > need ? scratch_cap - need : 0;
    uint8_t *z = scratch + need;
    size_t zn = 0;

    size_t p = 8;
    while (p + 8 <= len) {
        uint32_t clen = be32(src + p);
        const uint8_t *ctype4 = src + p + 4;
        const uint8_t *cdata  = src + p + 8;
        if (p + 12 + (size_t)clen > len) break;              /* truncated tail */

        if (!memcmp(ctype4, "PLTE", 4)) {
            uint32_t n = clen / 3; if (n > 256) n = 256;
            for (uint32_t i = 0; i < n; i++) {
                pal[i][0] = cdata[i*3]; pal[i][1] = cdata[i*3+1]; pal[i][2] = cdata[i*3+2];
            }
            have_pal = (int)n;
        } else if (!memcmp(ctype4, "tRNS", 4)) {
            if (h.ctype == 3) {
                for (uint32_t i = 0; i < clen && i < 256; i++) palא[i] = cdata[i];
            } else if (h.ctype == 0 && clen >= 2) {
                trns_grey = cdata[1];                         /* 8-bit sample */
            } else if (h.ctype == 2 && clen >= 6) {
                trns_r = cdata[1]; trns_g = cdata[3]; trns_b = cdata[5];
            }
        } else if (!memcmp(ctype4, "IDAT", 4)) {
            if (zn + clen > zcap) return PNG_ETOOBIG;
            memcpy(z + zn, cdata, clen);
            zn += clen;
        } else if (!memcmp(ctype4, "IEND", 4)) {
            break;
        }
        p += 12 + clen;
    }
    if (zn < 3) return PNG_EDATA;

    /* --- zlib wrapper: 2 header bytes, then raw DEFLATE ------------------ */
    if ((z[0] & 0x0F) != 8) return PNG_EDATA;                 /* not deflate  */
    if (z[1] & 0x20)        return PNG_EUNSUP;                /* preset dict  */
    size_t raw_n = 0;
    if (inflate_raw(z + 2, zn - 2, scratch, need, &raw_n) != 0) return PNG_EDATA;
    if (raw_n < need) return PNG_EDATA;

    /* --- unfilter, then convert --------------------------------------- */
    int bpp = (bits + 7) / 8; if (bpp < 1) bpp = 1;
    uint8_t *prev = 0;
    for (uint32_t y = 0; y < h.h; y++) {
        uint8_t *row = scratch + (size_t)y * (stride + 1);
        int f = row[0];
        uint8_t *cur = row + 1;
        unfilter(cur, prev, stride, bpp, f);
        prev = cur;
    }

    int step = h.depth == 16 ? 2 : 1;                          /* 16-bit: take the high byte */
    for (uint32_t y = 0; y < h.h; y++) {
        const uint8_t *row = scratch + (size_t)y * (stride + 1) + 1;
        uint32_t *o = dst + (size_t)y * h.w;
        for (uint32_t x = 0; x < h.w; x++) {
            int r, g, b, a = 255;
            if (h.ctype == 3) {
                uint32_t idx;
                if (h.depth == 8) idx = row[x];
                else {
                    uint32_t per = 8u / (uint32_t)h.depth;
                    uint32_t byte = row[x / per];
                    uint32_t shift = (per - 1 - (x % per)) * (uint32_t)h.depth;
                    idx = (byte >> shift) & ((1u << h.depth) - 1u);
                }
                if (!have_pal || idx >= (uint32_t)have_pal) { r = g = b = 0; }
                else { r = pal[idx][0]; g = pal[idx][1]; b = pal[idx][2]; a = palא[idx]; }
            } else if (h.ctype == 0) {
                r = g = b = row[(size_t)x * (size_t)step * 1];
                if (trns_grey >= 0 && r == trns_grey) a = 0;
            } else if (h.ctype == 4) {
                const uint8_t *s2 = row + (size_t)x * 2 * (size_t)step;
                r = g = b = s2[0];
                a = s2[step];
            } else if (h.ctype == 2) {
                const uint8_t *s2 = row + (size_t)x * 3 * (size_t)step;
                r = s2[0]; g = s2[step]; b = s2[2 * step];
                if (trns_r >= 0 && r == trns_r && g == trns_g && b == trns_b) a = 0;
            } else {  /* 6: RGBA */
                const uint8_t *s2 = row + (size_t)x * 4 * (size_t)step;
                r = s2[0]; g = s2[step]; b = s2[2 * step]; a = s2[3 * step];
            }
            o[x] = bgra(r, g, b, a);
        }
    }

    if (out_w) *out_w = h.w;
    if (out_h) *out_h = h.h;
    return PNG_OK;
}
