/* kernel/gfx/pix.c -- the compositor's pixel loops (pix.h). Built -O2 even in
 * the -O0 kernel: these are the loops a compose spends its time in, and none
 * of them is anything a debugger needs to step through. */
#include "gfx/pix.h"

void pix_over_row(uint32_t *dst, const uint32_t *src, int n, int swap)
{
    for (int i = 0; i < n; i++) {
        uint32_t s = src[i], a = s >> 24;
        if (a == 0) continue;
        if (swap) s = pix_swap_rb(s);
        dst[i] = a == 255u ? (s & 0x00ffffffu) : pix_over(s, dst[i]);
    }
}

void pix_mix_row(uint32_t *dst, const uint32_t *src, int n, uint32_t a, int swap)
{
    for (int i = 0; i < n; i++) {
        uint32_t s = swap ? pix_swap_rb(src[i]) : src[i];
        dst[i] = pix_mix(s, dst[i], a);
    }
}

void pix_blend_row(uint32_t *dst, const uint32_t *src, int n, int swap)
{
    for (int i = 0; i < n; i++) {
        uint32_t s = src[i], a = s >> 24;
        if (a == 0) continue;
        if (swap) s = pix_swap_rb(s);
        dst[i] = a == 255u ? (s & 0x00ffffffu) : pix_mix(s, dst[i], a);
    }
}

void pix_copy_row(uint32_t *dst, const uint32_t *src, int n, int swap)
{
    if (swap) for (int i = 0; i < n; i++) dst[i] = pix_swap_rb(src[i]) & 0x00ffffffu;
    else      for (int i = 0; i < n; i++) dst[i] = src[i] & 0x00ffffffu;
}

void pix_scale_row(uint32_t *dst, int n, const uint32_t *r0, const uint32_t *r1, int sw,
                   int32_t sx, int32_t step, uint32_t fy, uint32_t alpha, int swap)
{
    for (int i = 0; i < n; i++, sx += step) {
        int32_t s = sx < 0 ? 0 : sx;
        int x0 = s >> 16;
        uint32_t fx = ((uint32_t)s >> 8) & 0xffu;
        int x1 = x0 + 1;
        if (x0 >= sw - 1) { x0 = x1 = sw - 1; fx = 0; }
        uint32_t c = pix_lerp(pix_lerp(r0[x0], r0[x1], fx), pix_lerp(r1[x0], r1[x1], fx), fy);
        if (swap) c = pix_swap_rb(c);
        dst[i] = alpha >= 255u ? c : pix_mix(c, dst[i], alpha);
    }
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void pix_saturate_row(uint32_t *px, int n, uint32_t s, int swap)
{
    int ws = swap ? 0 : 16, wb = swap ? 16 : 0;       /* where red and blue sit */
    for (int i = 0; i < n; i++) {
        uint32_t v = px[i];
        int r = (int)((v >> ws) & 0xffu), g = (int)((v >> 8) & 0xffu), b = (int)((v >> wb) & 0xffu);
        int l = (77 * r + 150 * g + 29 * b) >> 8;
        r = clampi(l + (((r - l) * (int)s) >> 8), 0, 255);
        g = clampi(l + (((g - l) * (int)s) >> 8), 0, 255);
        b = clampi(l + (((b - l) * (int)s) >> 8), 0, 255);
        px[i] = (v & 0xff000000u) | ((uint32_t)r << ws) | ((uint32_t)g << 8) | ((uint32_t)b << wb);
    }
}

void pix_box_blur(uint32_t *px, int w, int h, int r, uint32_t *b, uint32_t *col)
{
    uint32_t win = (uint32_t)(2 * r + 1), m = pix_box_recip(win);

    /* horizontal: px -> b, a running sum along each row */
    for (int j = 0; j < h; j++) {
        const uint32_t *ra = px + (long)j * w;
        uint32_t *rb = b + (long)j * w;
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t v = ra[clampi(k, 0, w - 1)];
            sr += (v >> 16) & 0xffu; sg += (v >> 8) & 0xffu; sb += v & 0xffu;
        }
        for (int i = 0; i < w; i++) {
            rb[i] = pix_box_div(sr, m) << 16 | pix_box_div(sg, m) << 8 | pix_box_div(sb, m);
            uint32_t vo = ra[i - r < 0 ? 0 : i - r], vn = ra[i + r + 1 >= w ? w - 1 : i + r + 1];
            sr += ((vn >> 16) & 0xffu) - ((vo >> 16) & 0xffu);
            sg += ((vn >> 8) & 0xffu) - ((vo >> 8) & 0xffu);
            sb += (vn & 0xffu) - (vo & 0xffu);
        }
    }

    /* vertical: b -> px, a running sum per column, the rows walked in order */
    uint32_t *cr = col, *cg = col + w, *cb = col + 2 * w;
    for (int i = 0; i < w; i++) cr[i] = cg[i] = cb[i] = 0;
    for (int k = -r; k <= r; k++) {
        const uint32_t *row = b + (long)clampi(k, 0, h - 1) * w;
        for (int i = 0; i < w; i++) {
            uint32_t v = row[i];
            cr[i] += (v >> 16) & 0xffu; cg[i] += (v >> 8) & 0xffu; cb[i] += v & 0xffu;
        }
    }
    for (int j = 0; j < h; j++) {
        uint32_t *out = px + (long)j * w;
        for (int i = 0; i < w; i++)
            out[i] = pix_box_div(cr[i], m) << 16 | pix_box_div(cg[i], m) << 8 | pix_box_div(cb[i], m);
        const uint32_t *ro = b + (long)(j - r < 0 ? 0 : j - r) * w;
        const uint32_t *rn = b + (long)(j + r + 1 >= h ? h - 1 : j + r + 1) * w;
        for (int i = 0; i < w; i++) {
            uint32_t vo = ro[i], vn = rn[i];
            cr[i] += ((vn >> 16) & 0xffu) - ((vo >> 16) & 0xffu);
            cg[i] += ((vn >> 8) & 0xffu) - ((vo >> 8) & 0xffu);
            cb[i] += (vn & 0xffu) - (vo & 0xffu);
        }
    }
}
