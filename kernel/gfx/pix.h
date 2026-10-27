/* kernel/gfx/pix.h -- the compositor's pixel loops, two channels per multiply.
 *
 * Every compose spends its time here: a desktop window laid over the
 * wallpaper, an app window over that, a shadow, a blur. The loops these
 * replace unpacked each pixel through a function, divided by 255 three times
 * and packed it back -- in a kernel built -O0, where each of those is a call.
 * Built -O2 instead (pix.c is), a 1280 x 800 blend measured 11.6 ms -> 2.4 ms
 * and the box blur 17.2 ms -> 4.5 ms on the host, scalar as the kernel builds
 * them (`make pix-bench`, docs/DESKTOP_PERFORMANCE.md). Most of that is -O2
 * and the calls gone: the compiler makes a divide by 255 a multiply too.
 *
 * SWAR, not SIMD: red and blue sit 16 bits apart in a pixel word, so one
 * 32-bit multiply scales both, and a product of two bytes fits its 16-bit lane
 * (255 * 255 = 65025). Integer only, no FPU state -- the same code runs in the
 * kernel on both architectures and in host tests.
 *
 * EXACT, not approximate. floor(x / 255) is (x + 1 + (x >> 8)) >> 8 for every
 * x up to 65534, which covers every lane, so each function here returns the
 * very bits the loop it replaced did (tests/gfx/pix_test.c holds them to it).
 *
 * Pixels: 0xAARRGGBB words, alpha in the top byte. A destination word holds
 * the scanout's own byte order with the top byte ignored and written 0; when
 * that order is R,G,B,X (word 0x00BBGGRR) the source is put into it first with
 * pix_swap_rb -- blending treats every channel alike, so nothing else changes. */
#ifndef EMBLINK_PIX_H
#define EMBLINK_PIX_H
#include <stdint.h>

/* Exchange the red and blue bytes, keeping alpha and green. */
static inline uint32_t pix_swap_rb(uint32_t v)
{
    return (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
}

/* floor(lane / 255) in both 16-bit lanes of v (each lane at most 65534). */
static inline uint32_t pix_div255_2(uint32_t v)
{
    return ((v + 0x00010001u + ((v >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
}

static inline uint32_t pix_div255(uint32_t v)
{
    return (v + 1u + (v >> 8)) >> 8;
}

/* floor((s * a + d * (255 - a)) / 255) per channel, alpha byte 0. */
static inline uint32_t pix_mix(uint32_t s, uint32_t d, uint32_t a)
{
    uint32_t k = 255u - a;
    uint32_t rb = pix_div255_2((s & 0x00ff00ffu) * a + (d & 0x00ff00ffu) * k);
    uint32_t g = pix_div255(((s >> 8) & 0xffu) * a + ((d >> 8) & 0xffu) * k);
    return rb | (g << 8);
}

/* Premultiplied source over: s + floor(d * (255 - a) / 255) per channel,
 * clamped to 255 (a source brighter than its alpha is not premultiplied, and
 * the clamp is what the old loop did with it). Alpha byte 0. */
static inline uint32_t pix_over(uint32_t s, uint32_t d)
{
    uint32_t k = 255u - (s >> 24);
    uint32_t rb = (s & 0x00ff00ffu) + pix_div255_2((d & 0x00ff00ffu) * k);
    rb = (rb | (((rb & 0x01000100u) >> 8) * 0xffu)) & 0x00ff00ffu;
    uint32_t g = ((s >> 8) & 0xffu) + pix_div255(((d >> 8) & 0xffu) * k);
    if (g > 255u) g = 255u;
    return rb | (g << 8);
}

/* Rows. `swap` is 1 when the destination's order is R,G,B,X. */

/* Premultiplied over: alpha 0 leaves the destination, 255 replaces it. */
void pix_over_row(uint32_t *dst, const uint32_t *src, int n, int swap);
/* The source at a uniform alpha over the destination (a < 255). */
void pix_mix_row(uint32_t *dst, const uint32_t *src, int n, uint32_t a, int swap);
/* STRAIGHT (not premultiplied) alpha: each pixel at its own alpha. */
void pix_blend_row(uint32_t *dst, const uint32_t *src, int n, int swap);
/* The source's colour, alpha dropped. */
void pix_copy_row(uint32_t *dst, const uint32_t *src, int n, int swap);

/* (a * (256 - f) + b * f) >> 8 per channel, f in 0..256: the step of a
 * bilinear filter. The products fit their lanes (255 * 256 = 65280). */
static inline uint32_t pix_lerp(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t k = 256u - f;
    uint32_t rb = (((a & 0x00ff00ffu) * k + (b & 0x00ff00ffu) * f) >> 8) & 0x00ff00ffu;
    uint32_t g = ((((a >> 8) & 0xffu) * k + ((b >> 8) & 0xffu) * f) >> 8) & 0xffu;
    return rb | (g << 8);
}

/* A SCALED ROW, FILTERED: n destination pixels from the source rows `r0` and
 * `r1` (sw wide), `fy` (0..256) of the way from one to the other. The first
 * pixel samples source x `sx` (16.16, may be negative: clamped to the edge)
 * and each next one `step` further. The colour is mixed over the destination
 * at `alpha` (255: replaces it); source alpha is ignored. Window motion's
 * ghost: nearest-neighbour made the text in a shrinking window crawl. */
void pix_scale_row(uint32_t *dst, int n, const uint32_t *r0, const uint32_t *r1, int sw,
                   int32_t sx, int32_t step, uint32_t fy, uint32_t alpha, int swap);

/* VIBRANCY: each colour moved away from its own grey by `s` (Q8: 256 keeps
 * it, 435 is 1.7 times), clamped -- grey being the luma 0.30 R + 0.59 G +
 * 0.11 B, in eighths of 256. A blurred backdrop on its own goes muddy; frosted
 * glass that looks like glass saturates what shows through it, as the Mac's
 * materials do. `swap` as for the rows above: the word is R,G,B,X. */
#define PIX_VIBRANCY 435u
void pix_saturate_row(uint32_t *px, int n, uint32_t s, int swap);

/* floor(sum / win) for 0 <= sum <= 255 * win, win in [1, PIX_BOX_MAX_WIN]:
 * one multiply and a shift (pix_box_recip(win) is the multiplier). */
#define PIX_BOX_MAX_WIN 255
static inline uint32_t pix_box_recip(uint32_t win) { return (1u << 24) / win + 1u; }
static inline uint32_t pix_box_div(uint32_t sum, uint32_t m) { return (sum * m) >> 24; }

/* The box blur fb_blur_region draws, of a w x h block of 0x00RRGGBB pixels in
 * place: a horizontal pass of 2r+1 pixels then a vertical one, the edges
 * repeated, each mean floored. `scratch` holds w * h words. The vertical pass
 * keeps a running sum for every column and walks the rows, rather than
 * walking down each column in turn -- the same sums, read in memory order. */
void pix_box_blur(uint32_t *px, int w, int h, int r, uint32_t *scratch, uint32_t *colsums);

#endif
