/* soft_compose.c -- gpu_compose's operations on the CPU. See soft_compose.h. */
#include "gfx/soft_compose.h"

#define ONE 256                 /* 1.0 in the fixed point coverage, ring and falloff use */

int sc_intersect(struct gpu_rect *a, const struct gpu_rect *b)
{
    int32_t x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
    int32_t x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    int32_t y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    if (x1 <= x0 || y1 <= y0) { a->w = a->h = 0; return 0; }
    *a = (struct gpu_rect){ x0, y0, x1 - x0, y1 - y0 };
    return 1;
}

/* Where a call may draw: the rectangle, the clip and the surface. */
static int area(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *r, struct gpu_rect *out)
{
    struct gpu_rect s = { 0, 0, t->w, t->h };
    *out = *r;
    return sc_intersect(out, clip) && sc_intersect(out, &s);
}

static uint32_t *at(const struct sc_surface *t, int32_t x, int32_t y)
{
    return (uint32_t *)(t->px + (uint64_t)y * t->stride) + x;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* The distance from pixel (x, y)'s centre to a rounded rectangle, negative
 * inside, in 1/256 px: the shaders' q = |p - c| - half + r,
 * d = |max(q, 0)| + min(max(q.x, q.y), 0) - r. Outside the corner boxes
 * that is the larger of the two straight distances, with no root to take. */
static int32_t sdf(const struct gpu_rect *f, int32_t r, int32_t x, int32_t y)
{
    int32_t px = (x - f->x) * ONE + ONE / 2, py = (y - f->y) * ONE + ONE / 2;
    int32_t hx = f->w * (ONE / 2), hy = f->h * (ONE / 2), rr = r * ONE;
    int32_t ax = px > hx ? px - hx : hx - px, ay = py > hy ? py - hy : hy - py;
    int32_t qx = ax - hx + rr, qy = ay - hy + rr;
    if (qx <= 0 || qy <= 0) {
        int32_t dx = ax - hx, dy = ay - hy;
        return dx > dy ? dx : dy;
    }
    return (int32_t)isqrt64((uint64_t)((int64_t)qx * qx + (int64_t)qy * qy)) - rr;
}

static int32_t clamp1(int32_t v) { return v < 0 ? 0 : v > ONE ? ONE : v; }

static uint32_t chan(uint32_t c, int s) { return (c >> s) & 255u; }

/* src + dst * (1 - src.a): premultiplied over. */
static uint32_t over(uint32_t src, uint32_t dst)
{
    uint32_t inv = 255u - (src >> 24), out = 0;
    for (int s = 0; s < 32; s += 8) {
        uint32_t v = chan(src, s) + (chan(dst, s) * inv + 127u) / 255u;
        out |= (v > 255u ? 255u : v) << s;
    }
    return out;
}

/* Every channel times k / 256. */
static uint32_t scale(uint32_t c, uint32_t k)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) out |= ((chan(c, s) * k + ONE / 2) / ONE) << s;
    return out;
}

/* a * (1 - k) + b * k, channel by channel (k / 256). */
static uint32_t mix(uint32_t a, uint32_t b, uint32_t k)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) out |= ((chan(a, s) * (ONE - k) + chan(b, s) * k + ONE / 2) / ONE) << s;
    return out;
}

void sc_fill(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *r, uint32_t rgb)
{
    struct gpu_rect a;
    if (!area(t, clip, r, &a)) return;
    uint32_t v = 0xff000000u | (rgb & 0xffffffu);
    for (int32_t y = a.y; y < a.y + a.h; y++) {
        uint32_t *p = at(t, a.x, y);
        for (int32_t i = 0; i < a.w; i++) p[i] = v;
    }
}

/* The image's pixel under screen pixel (x, y): nearest when the sizes match,
 * bilinear between texel centres when scaled (the shaders' LINEAR sampler,
 * clamped at the edges). Premultiplied; alpha 255 when opaque. */
static uint32_t texel(const struct sc_surface *img, const struct gpu_draw *d, int32_t x, int32_t y)
{
    uint32_t c;
    if (d->src.w == d->dst.w && d->src.h == d->dst.h) {
        int32_t sx = d->src.x + (x - d->dst.x), sy = d->src.y + (y - d->dst.y);
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx >= img->w) sx = img->w - 1;
        if (sy >= img->h) sy = img->h - 1;
        c = *at(img, sx, sy);
    } else {
        /* u in 1/256 texel: src.x + (x + 0.5 - dst.x) * src.w / dst.w, less half a texel */
        int64_t u = (int64_t)d->src.x * ONE + ((int64_t)(x - d->dst.x) * ONE + ONE / 2) * d->src.w / d->dst.w - ONE / 2;
        int64_t v = (int64_t)d->src.y * ONE + ((int64_t)(y - d->dst.y) * ONE + ONE / 2) * d->src.h / d->dst.h - ONE / 2;
        int32_t u0 = (int32_t)(u >> 8), v0 = (int32_t)(v >> 8);
        uint32_t fu = (uint32_t)(u & 255), fv = (uint32_t)(v & 255);
        int32_t xs[2] = { u0, u0 + 1 }, ys[2] = { v0, v0 + 1 };
        for (int k = 0; k < 2; k++) {
            if (xs[k] < 0) xs[k] = 0;
            if (xs[k] >= img->w) xs[k] = img->w - 1;
            if (ys[k] < 0) ys[k] = 0;
            if (ys[k] >= img->h) ys[k] = img->h - 1;
        }
        uint32_t p00 = *at(img, xs[0], ys[0]), p10 = *at(img, xs[1], ys[0]);
        uint32_t p01 = *at(img, xs[0], ys[1]), p11 = *at(img, xs[1], ys[1]);
        if (d->opaque) { p00 |= 0xff000000u; p10 |= 0xff000000u; p01 |= 0xff000000u; p11 |= 0xff000000u; }
        c = 0;
        for (int s = 0; s < 32; s += 8) {
            uint32_t top = chan(p00, s) * (ONE - fu) + chan(p10, s) * fu;
            uint32_t bot = chan(p01, s) * (ONE - fu) + chan(p11, s) * fu;
            c |= ((top * (ONE - fv) + bot * fv + ONE * ONE / 2) / (ONE * ONE)) << s;
        }
    }
    return d->opaque ? c | 0xff000000u : c;
}

void sc_draw(const struct sc_surface *t, const struct gpu_rect *clip, const struct sc_surface *img,
             const struct gpu_draw *d)
{
    struct gpu_rect a;
    if (!area(t, clip, &d->dst, &a) || img->w <= 0 || img->h <= 0 || d->src.w <= 0 || d->src.h <= 0) return;
    struct gpu_rect f = d->frame.w > 0 && d->frame.h > 0 ? d->frame : d->dst;
    int32_t m = (f.w < f.h ? f.w : f.h) / 2, radius = d->radius > m ? m : d->radius;
    int plain = d->opaque && d->opacity == 255 && !radius && !d->ring;
    uint32_t ring = 0xff000000u | (d->ring_rgb & 0xffffffu), op = d->opacity;
    for (int32_t y = a.y; y < a.y + a.h; y++) {
        uint32_t *p = at(t, a.x, y);
        for (int32_t i = 0; i < a.w; i++) {
            int32_t x = a.x + i, dist = sdf(&f, radius, x, y);
            int32_t cov = clamp1(ONE / 2 - dist);
            if (!cov) continue;
            uint32_t c = texel(img, d, x, y);
            if (d->ring) {
                int32_t rg = clamp1(dist + (int32_t)d->ring * ONE + ONE / 2);
                if (rg) c = mix(c, ring, (uint32_t)rg);
            }
            if (plain) { p[i] = c; continue; }
            c = scale(c, (uint32_t)cov * op / 255u);
            p[i] = over(c, p[i]);
        }
    }
}

void sc_shadow(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *frame,
               const struct gpu_rect *cover, uint32_t radius, uint32_t width, uint32_t peak)
{
    if (!width || !peak || frame->w <= 0 || frame->h <= 0) return;
    struct gpu_rect q = { frame->x - (int32_t)width, frame->y - (int32_t)width,
                          frame->w + 2 * (int32_t)width, frame->h + 2 * (int32_t)width }, a;
    if (!area(t, clip, &q, &a)) return;
    int32_t m = (frame->w < frame->h ? frame->w : frame->h) / 2, r = (int32_t)radius > m ? m : (int32_t)radius;
    int has_cover = cover && cover->w > 0 && cover->h > 0;
    int32_t cm = has_cover ? (cover->w < cover->h ? cover->w : cover->h) / 2 : 0, cr = r > cm ? cm : r;
    for (int32_t y = a.y; y < a.y + a.h; y++) {
        uint32_t *p = at(t, a.x, y);
        for (int32_t i = 0; i < a.w; i++) {
            int32_t x = a.x + i, e = sdf(frame, r, x, y);
            if (e < -ONE / 2) e = -ONE / 2;
            e += ONE / 2;                                 /* how far the pixel's far edge is out */
            int32_t fall = clamp1(ONE - e / (int32_t)width);
            int32_t mask = has_cover ? ONE - clamp1(ONE / 2 - sdf(cover, cr, x, y)) : ONE;
            uint32_t alpha = (uint32_t)((int64_t)fall * fall / ONE * mask / ONE) * peak / ONE;   /* 0..255 */
            if (alpha) p[i] = over(alpha << 24, p[i]);
        }
    }
}

void sc_blur(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *r0, uint32_t radius,
             const struct gpu_rect *frame, uint32_t corner, uint32_t *scratch)
{
    struct gpu_rect s = { 0, 0, t->w, t->h }, r = *r0, a;
    if (!radius || !sc_intersect(&r, &s) || r.w <= 1 || r.h <= 1) return;
    a = r;
    if (!sc_intersect(&a, clip)) return;
    struct gpu_rect f = frame && frame->w > 0 && frame->h > 0 ? *frame : r;
    int32_t m = (f.w < f.h ? f.w : f.h) / 2, cr = frame ? ((int32_t)corner > m ? m : (int32_t)corner) : 0;
    int32_t rad = (int32_t)radius, win = 2 * rad + 1;
    /* horizontal, clamped to r: the screen there -> scratch */
    for (int32_t j = 0; j < r.h; j++) {
        const uint32_t *row = at(t, r.x, r.y + j);
        uint32_t *o = scratch + (uint64_t)j * r.w;
        int32_t sum[3] = { 0, 0, 0 };
        for (int32_t k = -rad; k <= rad; k++) {
            uint32_t v = row[k < 0 ? 0 : k >= r.w ? r.w - 1 : k];
            sum[0] += (int32_t)chan(v, 16); sum[1] += (int32_t)chan(v, 8); sum[2] += (int32_t)chan(v, 0);
        }
        for (int32_t i = 0; i < r.w; i++) {
            o[i] = ((uint32_t)(sum[0] / win) << 16) | ((uint32_t)(sum[1] / win) << 8) | (uint32_t)(sum[2] / win);
            int32_t old = i - rad, nw = i + rad + 1;
            uint32_t vo = row[old < 0 ? 0 : old], vn = row[nw >= r.w ? r.w - 1 : nw];
            sum[0] += (int32_t)chan(vn, 16) - (int32_t)chan(vo, 16);
            sum[1] += (int32_t)chan(vn, 8) - (int32_t)chan(vo, 8);
            sum[2] += (int32_t)chan(vn, 0) - (int32_t)chan(vo, 0);
        }
    }
    /* vertical, clamped to r: scratch -> the screen, inside the clip, cut to the frame */
    for (int32_t i = a.x - r.x; i < a.x - r.x + a.w; i++) {
        int32_t sum[3] = { 0, 0, 0 };
        for (int32_t k = -rad; k <= rad; k++) {
            uint32_t v = scratch[(uint64_t)(k < 0 ? 0 : k >= r.h ? r.h - 1 : k) * r.w + i];
            sum[0] += (int32_t)chan(v, 16); sum[1] += (int32_t)chan(v, 8); sum[2] += (int32_t)chan(v, 0);
        }
        for (int32_t j = 0; j < r.h; j++) {
            int32_t y = r.y + j, x = r.x + i;
            if (y >= a.y && y < a.y + a.h) {
                int32_t cov = clamp1(ONE / 2 - sdf(&f, cr, x, y));
                if (cov) {
                    uint32_t b = 0xff000000u | ((uint32_t)(sum[0] / win) << 16) | ((uint32_t)(sum[1] / win) << 8) |
                                 (uint32_t)(sum[2] / win);
                    uint32_t *p = at(t, x, y);
                    *p = cov == ONE ? b : over(scale(b, (uint32_t)cov), *p);
                }
            }
            int32_t old = j - rad, nw = j + rad + 1;
            uint32_t vo = scratch[(uint64_t)(old < 0 ? 0 : old) * r.w + i];
            uint32_t vn = scratch[(uint64_t)(nw >= r.h ? r.h - 1 : nw) * r.w + i];
            sum[0] += (int32_t)chan(vn, 16) - (int32_t)chan(vo, 16);
            sum[1] += (int32_t)chan(vn, 8) - (int32_t)chan(vo, 8);
            sum[2] += (int32_t)chan(vn, 0) - (int32_t)chan(vo, 0);
        }
    }
}
