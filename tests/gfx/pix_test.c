/* pix_test.c -- kernel/gfx/pix.c on the host: every row it writes is the row
 * the framebuffer's old per-pixel loops wrote, bit for bit.
 *
 * The references below are those loops' arithmetic, copied as it stood in
 * kernel/drivers/video/framebuffer.c before pix.c: unpack, divide by 255 per
 * channel, clamp, pack. Exhaustive where the space allows it (every alpha
 * against every source and destination byte; every box-blur sum), random
 * pictures for the rows and the blur.
 *
 *   pix_test           the checks (built with ASan/UBSan by `make pix-test`)
 *   pix_test --bench   old loops against new on a 1280 x 800 screen (`make pix-bench`) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "gfx/pix.h"
#include "sc_bridge.h"

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)

static uint32_t seed = 0x2545F491u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
/* The scanout word for a 0x00RRGGBB colour, and back: a BGR scanout keeps it,
 * an RGB one exchanges red and blue (framebuffer.c's fb_pack / fb_unpack). */
static uint32_t pack(uint32_t c, int rgb) { c &= 0xffffffu; return rgb ? RGB(c & 255, (c >> 8) & 255, c >> 16) : c; }
static uint32_t unpack(uint32_t v, int rgb) { v &= 0xffffffu; return rgb ? RGB(v & 255, (v >> 8) & 255, v >> 16) : v; }

/* ---- the old loops' arithmetic, one pixel at a time ---- */
static uint32_t ref_over(uint32_t s, uint32_t dn, int rgb) {     /* fb_blit_over */
    uint32_t a = s >> 24;
    if (a == 0) return dn;
    if (a == 255) return pack(s & 0xffffffu, rgb);
    uint32_t inv = 255 - a, d = unpack(dn, rgb);
    uint32_t r = ((s >> 16) & 0xFF) + (((d >> 16) & 0xFF) * inv) / 255;
    uint32_t g = ((s >> 8) & 0xFF) + (((d >> 8) & 0xFF) * inv) / 255;
    uint32_t b = (s & 0xFF) + ((d & 0xFF) * inv) / 255;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return pack(RGB(r, g, b), rgb);
}
static uint32_t ref_mix(uint32_t s, uint32_t dn, uint32_t alpha, int rgb) {   /* fb_blit_uniform */
    uint32_t inv = 255 - alpha, d = unpack(dn, rgb);
    uint32_t r = (((s >> 16) & 0xFF) * alpha + ((d >> 16) & 0xFF) * inv) / 255;
    uint32_t g = (((s >> 8) & 0xFF) * alpha + ((d >> 8) & 0xFF) * inv) / 255;
    uint32_t b = ((s & 0xFF) * alpha + (d & 0xFF) * inv) / 255;
    return pack(RGB(r, g, b), rgb);
}
static uint32_t ref_blend(uint32_t s, uint32_t dn, int rgb) {    /* fb_blit_blend */
    uint32_t a = s >> 24;
    if (a == 0) return dn;
    if (a == 255) return pack(s, rgb);
    return ref_mix(s, dn, a, rgb);
}
/* fb_blur_region's two passes, divides and all */
static void ref_blur(uint32_t *a, int cw, int ch, int radius) {
    uint32_t *b = malloc((size_t)cw * ch * 4);
    int win = 2 * radius + 1;
    for (int j = 0; j < ch; j++) {
        uint32_t *ra = &a[j * cw], *rb = &b[j * cw];
        int sr = 0, sg = 0, sb = 0;
        for (int k = -radius; k <= radius; k++) {
            uint32_t v = ra[k < 0 ? 0 : (k >= cw ? cw - 1 : k)];
            sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
        }
        for (int i = 0; i < cw; i++) {
            rb[i] = RGB(sr / win, sg / win, sb / win);
            int o = i - radius, n = i + radius + 1;
            uint32_t vo = ra[o < 0 ? 0 : o], vn = ra[n >= cw ? cw - 1 : n];
            sr += ((vn >> 16) & 0xFF) - ((vo >> 16) & 0xFF);
            sg += ((vn >> 8) & 0xFF) - ((vo >> 8) & 0xFF);
            sb += (vn & 0xFF) - (vo & 0xFF);
        }
    }
    for (int i = 0; i < cw; i++) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = -radius; k <= radius; k++) {
            uint32_t v = b[(k < 0 ? 0 : (k >= ch ? ch - 1 : k)) * cw + i];
            sr += (v >> 16) & 0xFF; sg += (v >> 8) & 0xFF; sb += v & 0xFF;
        }
        for (int j = 0; j < ch; j++) {
            a[j * cw + i] = RGB(sr / win, sg / win, sb / win);
            int o = j - radius, n = j + radius + 1;
            uint32_t vo = b[(o < 0 ? 0 : o) * cw + i], vn = b[(n >= ch ? ch - 1 : n) * cw + i];
            sr += ((vn >> 16) & 0xFF) - ((vo >> 16) & 0xFF);
            sg += ((vn >> 8) & 0xFF) - ((vo >> 8) & 0xFF);
            sb += (vn & 0xFF) - (vo & 0xFF);
        }
    }
    free(b);
}

/* Every alpha against every source and destination byte, each channel given a
 * different byte so a carry between lanes cannot hide. */
static uint32_t spread(uint32_t a, uint32_t v) { return a << 24 | v << 16 | ((v * 7 + 3) & 255) << 8 | ((v * 13 + 5) & 255); }

static int exhaustive_pixels(void) {
    for (uint32_t a = 0; a < 256; a++)
        for (uint32_t sv = 0; sv < 256; sv++)
            for (uint32_t dv = 0; dv < 256; dv++) {
                uint32_t s = spread(a, sv), d = spread(0, dv);
                /* premultiplied (sv scaled by a) and not (the clamp case) both */
                uint32_t sp = a << 24 | RGB(sv * a / 255, ((sv * 7 + 3) & 255) * a / 255, ((sv * 13 + 5) & 255) * a / 255);
                if (a && a != 255 && pix_over(sp, d) != ref_over(sp, d, 0)) return 1;
                if (a && a != 255 && pix_over(s, d) != ref_over(s, d, 0)) return 2;
                if (pix_mix(s, d, a) != ref_mix(s, d, a, 0)) return 3;
            }
    return 0;
}

static int rows_match(int rgb) {
    enum { N = 4096 };
    static uint32_t src[N], dst[N], ref[N];
    for (int round = 0; round < 64; round++) {
        for (int i = 0; i < N; i++) {
            uint32_t a = rnd() & 255;
            if (i % 5 == 0) a = 0; else if (i % 7 == 0) a = 255;
            uint32_t c = rnd() & 0xffffffu;
            uint32_t pm = RGB(((c >> 16) & 255) * a / 255, ((c >> 8) & 255) * a / 255, (c & 255) * a / 255);
            src[i] = a << 24 | (round & 1 ? c : pm);      /* odd rounds: not premultiplied */
            dst[i] = rnd() & 0xffffffu;
        }
        int n = 1 + (int)(rnd() % N);
        uint32_t alpha = rnd() % 255;
        memcpy(ref, dst, sizeof dst);
        for (int i = 0; i < n; i++) ref[i] = ref_over(src[i], ref[i], rgb);
        memcpy(ref + n, dst + n, (N - n) * 4u);
        uint32_t got[N]; memcpy(got, dst, sizeof dst);
        pix_over_row(got, src, n, rgb);
        if (memcmp(got, ref, sizeof got)) return 1;
        memcpy(got, dst, sizeof dst); memcpy(ref, dst, sizeof dst);
        for (int i = 0; i < n; i++) ref[i] = ref_mix(src[i], ref[i], alpha, rgb);
        pix_mix_row(got, src, n, alpha, rgb);
        if (memcmp(got, ref, sizeof got)) return 2;
        memcpy(got, dst, sizeof dst); memcpy(ref, dst, sizeof dst);
        for (int i = 0; i < n; i++) ref[i] = ref_blend(src[i], ref[i], rgb);
        pix_blend_row(got, src, n, rgb);
        if (memcmp(got, ref, sizeof got)) return 3;
        memcpy(got, dst, sizeof dst); memcpy(ref, dst, sizeof dst);
        for (int i = 0; i < n; i++) ref[i] = pack(src[i], rgb);
        pix_copy_row(got, src, n, rgb);
        if (memcmp(got, ref, sizeof got)) return 4;
    }
    return 0;
}

static int blur_matches(void) {
    static const int sizes[][2] = { {2, 2}, {3, 17}, {17, 3}, {64, 48}, {97, 61}, {200, 9}, {9, 200}, {320, 180} };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++)
        for (int r = 1; r <= 40; r += (r < 8 ? 1 : 7)) {
            int w = sizes[k][0], h = sizes[k][1];
            uint32_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4);
            uint32_t *tmp = malloc((size_t)w * h * 4), *col = malloc((size_t)w * 3 * 4);
            for (int i = 0; i < w * h; i++) a[i] = b[i] = rnd() & 0xffffffu;
            for (int i = 0; i < w * h; i += 3) a[i] = b[i] = 0xffffff;       /* hard edges */
            ref_blur(a, w, h, r);
            pix_box_blur(b, w, h, r, tmp, col);
            int bad = memcmp(a, b, (size_t)w * h * 4);
            free(a); free(b); free(tmp); free(col);
            if (bad) { printf("    blur differs at %dx%d radius %d\n", w, h, r); return 1; }
        }
    return 0;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int bench(void) {
    enum { W = 1280, H = 800 };
    uint32_t *src = malloc(W * H * 4), *dst = malloc(W * H * 4), *tmp = malloc(W * H * 4), *col = malloc(W * 12);
    for (int i = 0; i < W * H; i++) {
        uint32_t a = i % 7 == 0 ? 255 : i % 5 == 0 ? 0 : rnd() & 255, c = rnd();
        src[i] = a << 24 | RGB(((c >> 16) & 255) * a / 255, ((c >> 8) & 255) * a / 255, (c & 255) * a / 255);
        dst[i] = rnd() & 0xffffffu;
    }
    double t0 = now_ms();
    for (int k = 0; k < 10; k++) for (int i = 0; i < W * H; i++) dst[i] = ref_over(src[i], dst[i], 0);
    double t1 = now_ms();
    for (int k = 0; k < 10; k++) for (int j = 0; j < H; j++) pix_over_row(dst + j * W, src + j * W, W, 0);
    double t2 = now_ms();
    for (int k = 0; k < 3; k++) ref_blur(dst, W, H, 6);
    double t3 = now_ms();
    for (int k = 0; k < 3; k++) pix_box_blur(dst, W, H, 6, tmp, col);
    double t4 = now_ms();
    printf("1280 x 800 screen, host, scalar: over %.2f ms -> %.2f ms; box blur r=6 %.2f ms -> %.2f ms\n",
           (t1 - t0) / 10, (t2 - t1) / 10, (t3 - t2) / 3, (t4 - t3) / 3);
    /* A ghost: a 980 x 660 window drawn at 88% (an opening one), opaque.
     * Nearest-neighbour as framebuffer.c had it, against the filtered row. */
    enum { SW = 980, SH = 660, DW = 862, DH = 580 };
    uint32_t stepx = (SW << 16) / DW, stepy = (SH << 16) / DH;
    double t5 = now_ms();
    for (int k = 0; k < 10; k++)
        for (int y = 0; y < DH; y++) {
            uint32_t sy = ((uint32_t)y * stepy) >> 16;
            const uint32_t *srow = src + sy * W;
            uint32_t sx = 0, *p = dst + y * W;
            for (int x = 0; x < DW; x++, sx += stepx) p[x] = srow[sx >> 16] & 0xffffffu;
        }
    double t6 = now_ms();
    for (int k = 0; k < 10; k++)
        for (int y = 0; y < DH; y++) {
            int32_t sy = (int32_t)(((int64_t)(2 * y + 1) * stepy) / 2) - 32768;
            if (sy < 0) sy = 0;
            int r0 = sy >> 16, r1 = r0 + 1 < SH ? r0 + 1 : r0;
            pix_scale_row(dst + y * W, DW, src + r0 * W, src + r1 * W, SW,
                          (int32_t)stepx / 2 - 32768, (int32_t)stepx, ((uint32_t)sy >> 8) & 255, 255, 0);
        }
    double t7 = now_ms();
    printf("a 980 x 660 window at 88%%: nearest %.2f ms, bilinear %.2f ms\n", (t6 - t5) / 10, (t7 - t6) / 10);
    /* a LAYER: a 260 x 400 menu, its own alpha, at 94% and 0.6 opacity, and
     * at its own size -- what a menu's drop-in costs the compositor a frame */
    {
        enum { LW = 260, LH = 400 };
        uint32_t *lay = malloc((size_t)LW * LH * 4);
        for (int i = 0; i < LW * LH; i++) lay[i] = (i % 7 ? 0xe0u : 0xffu) << 24 | 0x00505050u;
        int dw = LW * 94 / 100, dh = LH * 94 / 100;
        uint32_t lsx = ((uint32_t)LW << 16) / (uint32_t)dw, lsy = ((uint32_t)LH << 16) / (uint32_t)dh;
        double l0 = now_ms();
        for (int k = 0; k < 50; k++)
            for (int y = 0; y < dh; y++) {
                int32_t sy = (int32_t)(((int64_t)(2 * y + 1) * lsy) / 2) - 32768;
                if (sy < 0) sy = 0;
                int r0 = sy >> 16, r1 = r0 + 1 < LH ? r0 + 1 : r0;
                pix_scale_over_row(dst + y * W, dw, lay + r0 * LW, lay + r1 * LW, LW,
                                   (int32_t)lsx / 2 - 32768, (int32_t)lsx, ((uint32_t)sy >> 8) & 255, 153, 1, 0);
            }
        double l1 = now_ms();
        for (int k = 0; k < 50; k++)
            for (int y = 0; y < dh; y++) {
                int32_t sy = (int32_t)(((int64_t)(2 * y + 1) * lsy) / 2) - 32768;
                pix_scale_over_row(dst + y * W, dw, lay + (sy < 0 ? 0 : sy >> 16) * LW, 0, LW,
                                   (int32_t)lsx / 2 - 32768, (int32_t)lsx, 0, 153, 0, 0);
            }
        double l2 = now_ms();
        for (int k = 0; k < 50; k++)
            for (int y = 0; y < LH; y++) pix_over_fade_row(dst + y * W, lay + y * LW, LW, 153, 0);
        double l3 = now_ms();
        printf("a 260 x 400 layer at 94%%, 0.6 opacity: bilinear %.3f ms, nearest %.3f ms; at its own size %.3f ms\n",
               (l1 - l0) / 50, (l2 - l1) / 50, (l3 - l2) / 50);
        free(lay);
    }
    free(src); free(dst); free(tmp); free(col);
    return 0;
}

/* The filter one channel at a time, as a textbook writes it. */
static uint32_t ref_lerp1(uint32_t a, uint32_t b, uint32_t f) { return (a * (256 - f) + b * f) >> 8; }
static uint32_t ref_bilinear(const uint32_t *r0, const uint32_t *r1, int sw, int32_t sx, uint32_t fy) {
    int32_t s = sx < 0 ? 0 : sx;
    int x0 = s >> 16; uint32_t fx = ((uint32_t)s >> 8) & 255; int x1 = x0 + 1;
    if (x0 >= sw - 1) { x0 = x1 = sw - 1; fx = 0; }
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t top = ref_lerp1((r0[x0] >> sh) & 255, (r0[x1] >> sh) & 255, fx);
        uint32_t bot = ref_lerp1((r1[x0] >> sh) & 255, (r1[x1] >> sh) & 255, fx);
        out |= ref_lerp1(top, bot, fy) << sh;
    }
    return out;
}

static int scale_matches(void) {
    int bad = 0;
    uint32_t r0[97], r1[97], dst[300], want[300];
    for (int trial = 0; trial < 400 && !bad; trial++) {
        int sw = 1 + (int)(rnd() % 97), n = 1 + (int)(rnd() % 300), swap = (int)(rnd() & 1);
        for (int i = 0; i < sw; i++) { r0[i] = rnd(); r1[i] = rnd(); }
        int32_t step = (int32_t)((((uint64_t)sw) << 16) / (uint64_t)n);
        int32_t sx = step / 2 - 32768;                       /* pixel centres */
        uint32_t fy = rnd() % 257, alpha = trial % 3 ? 255 : rnd() % 256;
        for (int i = 0; i < n; i++) dst[i] = want[i] = rnd() & 0xffffffu;
        for (int i = 0; i < n; i++) {
            uint32_t c = ref_bilinear(r0, r1, sw, sx + i * step, fy);
            if (swap) c = RGB(c & 255, (c >> 8) & 255, c >> 16);
            want[i] = alpha >= 255 ? c : pix_mix(c, want[i], alpha);
        }
        pix_scale_row(dst, n, r0, r1, sw, sx, step, fy, alpha, swap);
        for (int i = 0; i < n; i++) if (dst[i] != want[i]) { bad = 1; printf("    sw %d n %d at %d: %08x != %08x\n", sw, n, i, dst[i], want[i]); break; }
    }
    /* a flat colour stays that colour however it is scaled: no darkening at
     * the edges, no rounding drift */
    for (int i = 0; i < 97; i++) r0[i] = r1[i] = 0xff3c7ab9u;
    pix_scale_row(dst, 300, r0, r1, 97, -20000, 21000, 128, 255, 0);
    for (int i = 0; i < 300; i++) if (dst[i] != 0x003c7ab9u) { bad = 1; break; }
    return bad;
}

/* ---- a layer's row against the compositor's reference draw ----------------
 * soft_compose.c's sc_draw is the virgl shaders' letter in integers: a
 * premultiplied picture, filtered between texel centres when scaled, faded,
 * over. The layer row does the same in two-lane arithmetic, rounding each
 * step to the nearest level -- and both are held to exact arithmetic. */
static uint32_t premul_rnd(void) {
    uint32_t a = rnd() % 4 == 0 ? 0 : rnd() % 4 == 0 ? 255 : rnd() & 255;
    uint32_t r = a ? rnd() % (a + 1) : 0, g = a ? rnd() % (a + 1) : 0, b = a ? rnd() % (a + 1) : 0;
    return a << 24 | r << 16 | g << 8 | b;
}
static int layer_rows_match(void) {
    enum { SW = 61, SH = 37, DWM = 140, DHM = 90 };
    static uint32_t img[SW * SH], a[DWM * DHM], b[DWM * DHM], o[DWM * DHM];
    int worst = 0;
    double off_row = 0, off_ref = 0;   /* each, against exact arithmetic */
    for (int trial = 0; trial < 300; trial++) {
        int sw = 2 + (int)(rnd() % (SW - 1)), sh = 2 + (int)(rnd() % (SH - 1));
        int dw = trial % 5 == 0 ? sw : 1 + (int)(rnd() % DWM), dh = trial % 5 == 0 ? sh : 1 + (int)(rnd() % DHM);
        uint32_t op = trial % 3 == 0 ? 255 : rnd() & 255;
        for (int i = 0; i < sw * sh; i++) img[i] = premul_rnd();
        for (int i = 0; i < dw * dh; i++) a[i] = b[i] = o[i] = 0xff000000u | (rnd() & 0xffffffu);
        sc_bridge_draw(a, dw, dh, img, sw, sh, op);      /* the reference */
        /* the row, as fb_blit_scaled_over walks it */
        if (dw == sw && dh == sh) {
            for (int y = 0; y < dh; y++) pix_over_fade_row(b + y * dw, img + y * sw, dw, op, 0);
        } else {
            uint32_t stepx = ((uint32_t)sw << 16) / (uint32_t)dw, stepy = ((uint32_t)sh << 16) / (uint32_t)dh;
            int32_t sx0 = (int32_t)(((int64_t)1 * stepx) / 2) - 32768;
            for (int y = 0; y < dh; y++) {
                int32_t sy = (int32_t)(((int64_t)(2 * y + 1) * stepy) / 2) - 32768;
                if (sy < 0) sy = 0;
                int32_t r0 = sy >> 16, r1 = r0 + 1;
                uint32_t fy = ((uint32_t)sy >> 8) & 0xffu;
                if (r0 >= sh - 1) { r0 = r1 = sh - 1; fy = 0; }
                pix_scale_over_row(b + y * dw, dw, img + r0 * sw, img + r1 * sw, sw, sx0, (int32_t)stepx, fy, op, 1, 0);
            }
        }
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
            double u = x, v = y;
            if (dw != sw || dh != sh) { u = (x + 0.5) * sw / dw - 0.5; v = (y + 0.5) * sh / dh - 0.5; }
            int u0 = (int)floor(u), v0 = (int)floor(v); double fu = u - u0, fv = v - v0;
            int xs[2] = { u0, u0 + 1 }, ys[2] = { v0, v0 + 1 };
            for (int k = 0; k < 2; k++) { if (xs[k] < 0) xs[k] = 0; if (xs[k] >= sw) xs[k] = sw - 1;
                                          if (ys[k] < 0) ys[k] = 0; if (ys[k] >= sh) ys[k] = sh - 1; }
            double ch[4];
            for (int c = 0; c < 4; c++) {
                int sf = c * 8;
                double p00 = (img[ys[0] * sw + xs[0]] >> sf) & 255, p10 = (img[ys[0] * sw + xs[1]] >> sf) & 255;
                double p01 = (img[ys[1] * sw + xs[0]] >> sf) & 255, p11 = (img[ys[1] * sw + xs[1]] >> sf) & 255;
                ch[c] = ((p00 * (1 - fu) + p10 * fu) * (1 - fv) + (p01 * (1 - fu) + p11 * fu) * fv) * op / 255.0;
            }
            for (int c = 0; c < 3; c++) {
                int sf = c * 8;
                double want = ch[c] + ((o[y * dw + x] >> sf) & 255) * (1 - ch[3] / 255.0);
                double e1 = fabs((double)((b[y * dw + x] >> sf) & 255) - want), e2 = fabs((double)((a[y * dw + x] >> sf) & 255) - want);
                if (e1 > off_row) off_row = e1;
                if (e2 > off_ref) off_ref = e2;
            }
        }
        for (int i = 0; i < dw * dh; i++)
            for (int sh8 = 0; sh8 < 24; sh8 += 8) {
                int e = (int)((a[i] >> sh8) & 255) - (int)((b[i] >> sh8) & 255);
                if (e < 0) e = -e;
                if (e > worst) worst = e;
            }
    }
    printf("        (the most a channel differs from sc_draw: %d; from exact arithmetic: the row %.2f, sc_draw %.2f)\n",
           worst, off_row, off_ref);
    /* Both quantise the filter's position to 1/256 of a texel, which between
     * two neighbours far apart is a level an axis: three from exact is the
     * floor for either. The row must be no further than the reference. */
    return off_row > off_ref + 0.5 || worst > 4;
}
static int fade_exact(void) {
    for (uint32_t o = 0; o < 256; o++)
        for (uint32_t v = 0; v < 256; v++) {
            uint32_t c = v << 24 | (255 - v) << 16 | (v / 2) << 8 | (v / 3), f = pix_fade4(c, o);
            for (int s = 0; s < 32; s += 8)
                if (((f >> s) & 255) != (((c >> s) & 255) * o + 127) / 255) return 1;
        }
    return 0;
}

static int saturate_matches(void) {
    int bad = 0;
    uint32_t row[256], want[256];
    for (int trial = 0; trial < 200 && !bad; trial++) {
        uint32_t s = trial % 4 == 0 ? 256 : 128 + rnd() % 512;
        int swap = trial & 1;
        for (int i = 0; i < 256; i++) row[i] = want[i] = rnd();
        for (int i = 0; i < 256; i++) {
            uint32_t v = want[i];
            int R = swap ? (int)(v & 255) : (int)((v >> 16) & 255), G = (int)((v >> 8) & 255);
            int B = swap ? (int)((v >> 16) & 255) : (int)(v & 255);
            int L = (77 * R + 150 * G + 29 * B) / 256;
            int c[3] = { R, G, B };
            for (int k = 0; k < 3; k++) {
                int x = L + (int)floor((double)(c[k] - L) * s / 256.0);
                c[k] = x < 0 ? 0 : x > 255 ? 255 : x;
            }
            want[i] = (v & 0xff000000u) | (uint32_t)(swap ? c[2] << 16 | c[1] << 8 | c[0] : c[0] << 16 | c[1] << 8 | c[2]);
        }
        pix_saturate_row(row, 256, s, swap);
        for (int i = 0; i < 256; i++) if (row[i] != want[i]) { bad = 1; printf("    %08x != %08x (s %u)\n", row[i], want[i], s); break; }
    }
    uint32_t grey[3] = { 0x00808080u, 0x00000000u, 0x00ffffffu };
    pix_saturate_row(grey, 3, PIX_VIBRANCY, 0);
    if (grey[0] != 0x00808080u || grey[1] || grey[2] != 0x00ffffffu) bad = 1;   /* grey has no colour to lose */
    return bad;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) return bench();
    printf("=== pix: the compositor's pixel loops ===\n");
    int div_ok = 1;
    for (uint32_t x = 0; x <= 65534 && div_ok; x++) div_ok = pix_div255(x) == x / 255;
    for (uint32_t x = 0; x <= 65025 && div_ok; x += 7) {
        uint32_t y = 65025 - x;
        div_ok = pix_div255_2(x | y << 16) == ((x / 255) | (y / 255) << 16);
    }
    CHECK(div_ok, "floor(x / 255) is exact for every lane value, alone and in pairs");
    CHECK(exhaustive_pixels() == 0, "over and mix equal the old arithmetic for every alpha, source and destination byte");
    int box_ok = 1;
    for (uint32_t win = 1; win <= PIX_BOX_MAX_WIN && box_ok; win++) {
        uint32_t m = pix_box_recip(win);
        for (uint32_t sum = 0; sum <= 255 * win && box_ok; sum++) box_ok = pix_box_div(sum, m) == sum / win;
    }
    CHECK(box_ok, "the box mean by reciprocal is the divide, for every window up to 255 and every sum");
    CHECK(pix_swap_rb(0x80123456u) == 0x80563412u, "red and blue exchange, alpha and green stay");
    CHECK(rows_match(0) == 0, "rows on a B,G,R,X scanout: over, uniform mix, straight blend, copy");
    CHECK(rows_match(1) == 0, "rows on an R,G,B,X scanout, the source put into its order");
    CHECK(blur_matches() == 0, "the box blur equals the old two passes, every size and radius tried");
    CHECK(saturate_matches() == 0, "vibrancy is the textbook saturation, either byte order; grey, black and white stay");
    CHECK(fade_exact() == 0, "a premultiplied pixel faded is every channel, alpha too, times o / 255, to the nearest level");
    CHECK(layer_rows_match() == 0, "a layer's row -- its own alpha, scaled, faded, over -- is as close to exact arithmetic as the reference draw, and within four levels of it");
    CHECK(scale_matches() == 0, "a filtered scaled row is the textbook bilinear, any scale, alpha or order; a flat colour stays flat");
    printf("=== pix: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
