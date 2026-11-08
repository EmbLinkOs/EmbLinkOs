/* The blur behind a window that shows through. See backdrop.h. */
#include "backdrop.h"
#include "include/kmalloc.h"

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* One box pass along rows (step 1) or columns (step `stride`): the mean of
 * 2r+1 pixels, the ends repeated. Integers only: a reciprocal, not a divide,
 * for every pixel -- under emulation a divide is dear. */
static void box_pass(const uint32_t *in, uint32_t *out, int lines, int len, int line_step, int step, int r) {
    uint32_t win = (uint32_t)(2 * r + 1), inv = (65536u + win / 2) / win;
    for (int l = 0; l < lines; l++) {
        const uint32_t *a = in + (long)l * line_step;
        uint32_t *b = out + (long)l * line_step;
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t v = a[(long)imax(0, imin(len - 1, k)) * step];
            sr += (v >> 16) & 255; sg += (v >> 8) & 255; sb += v & 255;
        }
        for (int i = 0; i < len; i++) {
            b[(long)i * step] = ((sr * inv + 32768) >> 16) << 16 | ((sg * inv + 32768) >> 16) << 8 | ((sb * inv + 32768) >> 16);
            uint32_t vo = a[(long)imax(0, i - r) * step], vn = a[(long)imin(len - 1, i + r + 1) * step];
            sr += ((vn >> 16) & 255) - ((vo >> 16) & 255);
            sg += ((vn >> 8) & 255) - ((vo >> 8) & 255);
            sb += (vn & 255) - (vo & 255);
        }
    }
}

int backdrop_reach(int radius) { return radius < 1 ? 0 : 2 * ((radius + 1) / 2); }

int backdrop_blur(const uint32_t *src, uint32_t *dst, int stride, int dx0, int dy0, int dx1, int dy1,
                  int x0, int y0, int x1, int y1, int radius) {
    x0 = imax(x0, dx0); y0 = imax(y0, dy0); x1 = imin(x1, dx1); y1 = imin(y1, dy1);
    if (x1 <= x0 || y1 <= y0) return 0;
    if (radius < 1) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) dst[(long)y * stride + x] = src[(long)y * stride + x] & 0xffffffu;
        return 0;
    }
    /* Each pass is wrong only within r of an edge it does not share with the
     * domain, and two passes each way carry that 2r in: read 2r further. */
    int r = (radius + 1) / 2, m = backdrop_reach(radius);
    int ex0 = imax(dx0, x0 - m), ey0 = imax(dy0, y0 - m), ex1 = imin(dx1, x1 + m), ey1 = imin(dy1, y1 + m);
    int ew = ex1 - ex0, eh = ey1 - ey0;
    uint32_t *a = kmalloc((unsigned long)ew * eh * 4), *b = a ? kmalloc((unsigned long)ew * eh * 4) : 0;
    if (!a || !b) { if (a) kfree(a); return -1; }
    for (int y = 0; y < eh; y++)
        for (int x = 0; x < ew; x++) a[(long)y * ew + x] = src[(long)(ey0 + y) * stride + ex0 + x];
    for (int pass = 0; pass < 2; pass++) {
        box_pass(a, b, eh, ew, ew, 1, r);       /* along each row */
        box_pass(b, a, ew, eh, 1, ew, r);       /* down each column */
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) dst[(long)y * stride + x] = a[(long)(y - ey0) * ew + (x - ex0)];
    kfree(b); kfree(a);
    return 0;
}
