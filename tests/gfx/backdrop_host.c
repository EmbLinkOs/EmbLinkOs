/* backdrop_host.c -- the compositor's backdrop blur (kernel/gfx/backdrop.c)
 * on the host: blurring part of a picture gives exactly the pixels blurring
 * all of it gives there, so the compositor can redo only what changed. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __KMALLOC_H__
static long live;
static void *kmalloc(uint64_t n) { void *p = malloc(n); live += p != NULL; return p; }
static void kfree(void *p) { if (p) live--; free(p); }
#include "../../kernel/gfx/backdrop.c"

static unsigned checks, failures;
#define CHECK(x, what) do { checks++; if (!(x)) { failures++; printf("  FAIL: %s (line %d)\n", what, __LINE__); } \
                            else printf("  ok:   %s\n", what); } while (0)

int main(void) {
    printf("=== backdrop blur ===\n");
    enum { W = 97, H = 61 };
    static uint32_t src[W * H], whole[W * H], part[W * H];
    uint32_t seed = 12345;
    for (int i = 0; i < W * H; i++) { seed = seed * 1103515245u + 12345u; src[i] = seed >> 8 & 0xffffffu; }
    for (int y = 20; y < 30; y++) for (int x = 0; x < W; x++) src[y * W + x] = 0xffffff;   /* a bright band */

    CHECK(backdrop_blur(src, whole, W, 0, 0, W, H, 0, 0, W, H, 12) == 0, "the whole picture, radius 12");
    bool same = true;
    static const int rects[][4] = {{0, 0, 10, 7}, {40, 25, 41, 26}, {30, 0, 70, 61}, {0, 50, 97, 61}, {85, 5, 97, 60}, {13, 17, 60, 33}};
    for (unsigned k = 0; k < sizeof rects / sizeof rects[0]; k++) {
        memset(part, 0xab, sizeof part);
        backdrop_blur(src, part, W, 0, 0, W, H, rects[k][0], rects[k][1], rects[k][2], rects[k][3], 12);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            bool in = x >= rects[k][0] && x < rects[k][2] && y >= rects[k][1] && y < rects[k][3];
            if (in ? part[y * W + x] != whole[y * W + x] : part[y * W + x] != 0xabababab) same = false;
        }
    }
    CHECK(same, "any part, corner, edge or middle, is exactly the whole's there -- and nothing outside it is touched");
    uint32_t flat[16 * 16], out[16 * 16];
    for (int i = 0; i < 256; i++) flat[i] = 0x336699;
    backdrop_blur(flat, out, 16, 0, 0, 16, 16, 0, 0, 16, 16, 9);
    same = true;
    for (int i = 0; i < 256; i++) same = same && out[i] == 0x336699;
    CHECK(same, "a flat colour stays itself, edges and all");
    int lo = 255, hi = 0;
    for (int y = 0; y < H; y++) { int v = whole[y * W + 48] & 255; if (v < lo) lo = v; if (v > hi) hi = v; }
    CHECK(hi - lo < 200 && (whole[25 * W + 48] & 255) > (whole[5 * W + 48] & 255), "it is blurred: noise evened out, the band still lighter");
    backdrop_blur(src, part, W, 0, 0, W, H, 0, 0, W, H, 0);
    CHECK(!memcmp(part, src, sizeof src), "radius 0 copies");
    CHECK(backdrop_blur(src, part, W, 0, 0, W, H, 50, 50, 40, 40, 8) == 0, "an empty rect is nothing to do");
    /* a domain inside the buffer: the same as blurring that crop on its own */
    enum { CW = 40, CH = 30, OX = 31, OY = 17 };
    static uint32_t crop[CW * CH], crop_blur[CW * CH], in_place[W * H];
    for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) crop[y * CW + x] = src[(OY + y) * W + OX + x];
    backdrop_blur(crop, crop_blur, CW, 0, 0, CW, CH, 0, 0, CW, CH, 10);
    backdrop_blur(src, in_place, W, OX, OY, OX + CW, OY + CH, OX + 5, OY + 3, OX + CW, OY + 20, 10);
    same = true;
    for (int y = 3; y < 20; y++) for (int x = 5; x < CW; x++) same = same && in_place[(OY + y) * W + OX + x] == crop_blur[y * CW + x];
    CHECK(same, "a domain inside the buffer (a window part off the screen) reads nothing outside it");
    CHECK(backdrop_reach(12) == 12 && backdrop_reach(5) == 6 && backdrop_reach(0) == 0, "the reach of a blur");
    CHECK(live == 0, "scratch memory is given back");
    printf("=== backdrop: %u checks, %u failures ===\n", checks, failures);
    return failures ? 1 : 0;
}
