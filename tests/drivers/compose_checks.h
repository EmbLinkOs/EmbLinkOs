/* compose_checks.h -- what any struct gpu_compose must draw, pixel for pixel.
 *
 * One suite for every backend: virgl on a real virglrenderer
 * (virgl_compose_test.c) and the Intel blitter with its software half on a
 * model of the engine (intel_compose_test.c). A backend draws into a W x H
 * screen whose pixels the test reads back through gc->read into `screen`:
 * fills, opaque and premultiplied images, opacity, rounded corners and the
 * border ring, the shadow and its cover, the blur against the CPU
 * compositor's own box blur (square and cut to a rounded frame), scaling,
 * partial image updates and drawing confined to the damage.
 *
 * Before including it: W, H, `static uint32_t screen[W * H]`, an int
 * `failures`, and `static uint32_t *check_pixels(size_t n)` handing out
 * memory for n image pixels that the backend's engine can reach (the
 * blitter reads images where they lie). Call compose_checks(gc) once the
 * backend is up. */
#ifndef EMBLINK_TEST_COMPOSE_CHECKS_H
#define EMBLINK_TEST_COMPOSE_CHECKS_H
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "drivers/video/gpu.h"

#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)

static const struct gpu_compose *gc;

static void readback(void)
{
    struct gpu_rect all = { 0, 0, W, H };
    assert(gc->read(&all) == 0);
}

static uint32_t px(int x, int y) { return screen[y * W + x] & 0xffffffu; }
static int chan(uint32_t c, int s) { return (int)((c >> s) & 255u); }
static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = chan(a, s) - chan(b, s);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

static void frame(const struct gpu_rect *damage, uint32_t background)
{
    assert(gc->begin(damage) == 0);
    gc->fill(damage, background);
}

static void compose_checks(const struct gpu_compose *ops)
{
    gc = ops;
    struct gpu_rect all = { 0, 0, W, H };

    /* 1. Fills, and where they land: a red block at (10, 20), 30 x 10. */
    frame(&all, 0x000000);
    struct gpu_rect red = { 10, 20, 30, 10 };
    gc->fill(&red, 0xff0000);
    assert(gc->end() == 0);
    readback();
    CHECK(px(10, 20) == 0xff0000 && px(39, 29) == 0xff0000, "fill corners %06x %06x", px(10, 20), px(39, 29));
    CHECK(px(9, 20) == 0 && px(10, 19) == 0 && px(40, 29) == 0 && px(39, 30) == 0, "fill bleeds: %06x %06x %06x %06x",
          px(9, 20), px(10, 19), px(40, 29), px(39, 30));

    /* 2. An opaque image, unscaled: every pixel where it belongs, top row on top. */
    uint32_t *pic = check_pixels(64 * 40);
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 64; x++)
            pic[y * 64 + x] = 0x80000000u | ((uint32_t)(x * 4) << 16) | ((uint32_t)(y * 6) << 8) | 0x40u;   /* alpha 0x80: ignored */
    uint32_t pic_id;
    assert(gc->image_create(pic, 64, 40, 256, &pic_id) == 0);
    frame(&all, 0x000000);
    struct gpu_draw d = { pic_id, { 0, 0, 64, 40 }, { 100, 50, 64, 40 }, 1, 255, 0, 0, { 0, 0, 0, 0 }, 0 };
    gc->draw(&d);
    assert(gc->end() == 0);
    readback();
    int exact = 1;
    for (int y = 0; y < 40 && exact; y++)
        for (int x = 0; x < 64 && exact; x++)
            if (px(100 + x, 50 + y) != (pic[y * 64 + x] & 0xffffffu)) {
                exact = 0;
                printf("  image (%d,%d): got %06x want %06x\n", x, y, px(100 + x, 50 + y), pic[y * 64 + x] & 0xffffffu);
            }
    CHECK(exact, "opaque image not exact");
    CHECK(px(99, 50) == 0 && px(164, 50) == 0, "image bleeds");

    /* 3. Premultiplied over: (0, 0, 128, 128) over white = (127, 127, 255). */
    uint32_t *half = check_pixels(16 * 16);
    for (int i = 0; i < 256; i++) half[i] = 0x80000080u;
    uint32_t half_id;
    assert(gc->image_create(half, 16, 16, 64, &half_id) == 0);
    frame(&all, 0xffffff);
    struct gpu_draw o = { half_id, { 0, 0, 16, 16 }, { 5, 5, 16, 16 }, 0, 255, 0, 0, { 0, 0, 0, 0 }, 0 };
    gc->draw(&o);
    assert(gc->end() == 0);
    readback();
    CHECK(near(px(10, 10), 0x7f7fff, 1), "premultiplied over: %06x", px(10, 10));

    /* 4. Opacity: the opaque picture at 50% over black. */
    frame(&all, 0x000000);
    struct gpu_draw op = d;
    op.opacity = 128;
    gc->draw(&op);
    assert(gc->end() == 0);
    readback();
    uint32_t want = pic[10 * 64 + 20] & 0xffffffu;
    uint32_t got = px(120, 60);
    CHECK(near(got, ((chan(want, 16) * 128 / 255) << 16) | ((chan(want, 8) * 128 / 255) << 8) | (chan(want, 0) * 128 / 255), 2),
          "opacity: %06x from %06x", got, want);

    /* 5. Rounded corners and the ring: a white window, radius 8, a 1px blue ring, on red. */
    uint32_t *white = check_pixels(80 * 60);
    for (int i = 0; i < 80 * 60; i++) white[i] = 0xffffffffu;
    uint32_t white_id;
    assert(gc->image_create(white, 80, 60, 320, &white_id) == 0);
    frame(&all, 0xff0000);
    struct gpu_draw rw = { white_id, { 0, 0, 80, 60 }, { 200, 100, 80, 60 }, 1, 255, 8, 1, { 200, 100, 80, 60 }, 0x0000ff };
    gc->draw(&rw);
    assert(gc->end() == 0);
    readback();
    CHECK(px(200, 100) == 0xff0000 && px(279, 159) == 0xff0000, "corner not cut: %06x %06x", px(200, 100), px(279, 159));
    CHECK(near(px(240, 100), 0x0000ff, 8) && near(px(200, 130), 0x0000ff, 8), "ring missing: %06x %06x", px(240, 100), px(200, 130));
    CHECK(px(240, 130) == 0xffffff, "interior: %06x", px(240, 130));
    CHECK(px(202, 102) != 0xffffff && px(202, 102) != 0xff0000, "no antialiased arc at (202,102): %06x", px(202, 102));

    /* 6. The shadow: darkest at the frame, gone `width` out -- and none under
     * its cover, the window casting it, even where the frame (cast 3px down)
     * reaches under it: a glass window must not show its own shadow. */
    frame(&all, 0xffffff);
    struct gpu_rect sf = { 60, 123, 60, 40 }, cover = { 60, 120, 60, 40 };
    gc->shadow(&sf, &cover, 0, 10, 128);
    assert(gc->end() == 0);
    readback();
    int edge = chan(px(59, 140), 0), mid = chan(px(55, 140), 0), far = chan(px(48, 140), 0);
    CHECK(edge < mid && mid < 255 && far == 255, "shadow falloff: edge %d mid %d far %d", edge, mid, far);
    /* The edge pixel is one out, as fb_glow_rect's first: peak * (9/10)^2. */
    CHECK(edge >= 255 - 104 - 3 && edge <= 255 - 104 + 3, "shadow edge strength %d", edge);
    CHECK(px(90, 140) == 0xffffff && px(90, 159) == 0xffffff, "shadow under its cover: %06x %06x", px(90, 140), px(90, 159));
    CHECK(chan(px(90, 161), 0) <= 255 - 128 + 3, "no full shadow below the cover: %06x", px(90, 161));

    /* 7. The blur, against the CPU compositor's box blur (fb_blur_region:
     * horizontal then vertical, each clamped to the region, truncating) on
     * the same pixels: a white block off-centre in both directions, so a
     * pass that ran the wrong way or a copy that turned rows over shows. */
    frame(&all, 0x000000);
    struct gpu_rect wl = { 150, 26, 14, 9 };
    gc->fill(&wl, 0xffffff);
    struct gpu_rect br = { 140, 20, 40, 30 };
    gc->blur(&br, 3, 0, 0);
    assert(gc->end() == 0);
    readback();
    static int src[30][40], hb[30][40], ref[30][40];
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++)
            src[y][x] = (140 + x >= wl.x && 140 + x < wl.x + wl.w && 20 + y >= wl.y && 20 + y < wl.y + wl.h) ? 255 : 0;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) {
            int sum = 0;
            for (int k = -3; k <= 3; k++) sum += src[y][x + k < 0 ? 0 : x + k > 39 ? 39 : x + k];
            hb[y][x] = sum / 7;
        }
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) {
            int sum = 0;
            for (int k = -3; k <= 3; k++) sum += hb[y + k < 0 ? 0 : y + k > 29 ? 29 : y + k][x];
            ref[y][x] = sum / 7;
        }
    int ref_bad = 0;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) {
            int have = chan(px(140 + x, 20 + y), 0), expect = ref[y][x];
            if (have - expect > 3 || expect - have > 3) {
                if (ref_bad++ < 8) printf("  blur (%d,%d) have %d want %d\n", 140 + x, 20 + y, have, expect);
            }
        }
    if (ref_bad && getenv("VGC_VERBOSE"))
        for (int y = 0; y < 30; y++) {
            for (int x = 0; x < 40; x++) printf("%c", " .:-=+*#%@"[chan(px(140 + x, 20 + y), 0) * 9 / 255]);
            printf("   ");
            for (int x = 0; x < 40; x++) printf("%c", " .:-=+*#%@"[ref[y][x] * 9 / 255]);
            puts("");
        }
    CHECK(!ref_bad, "blur differs from the CPU box blur at %d pixels", ref_bad);
    CHECK(px(139, 35) == 0 && px(180, 35) == 0 && px(160, 19) == 0 && px(160, 50) == 0,
          "blur outside its rect: %06x %06x %06x %06x", px(139, 35), px(180, 35), px(160, 19), px(160, 50));

    /* 7b. A blur cut to a rounded frame: the corner outside the arc keeps
     * what was there, the inside is frosted. */
    frame(&all, 0x000000);
    struct gpu_rect stripes = { 200, 20, 40, 40 };
    gc->fill(&stripes, 0xffffff);
    struct gpu_rect stripe = { 220, 20, 20, 40 };
    gc->fill(&stripe, 0x000000);
    struct gpu_rect fr = { 200, 20, 40, 40 };
    gc->blur(&fr, 3, &fr, 10);
    assert(gc->end() == 0);
    readback();
    CHECK(px(200, 20) == 0xffffff && px(239, 59) == 0x000000, "rounded blur touched its corners: %06x %06x",
          px(200, 20), px(239, 59));
    CHECK(chan(px(219, 40), 0) > 80 && chan(px(219, 40), 0) < 200 && chan(px(220, 40), 0) > 50 &&
          chan(px(220, 40), 0) < 180, "rounded blur not frosted inside: %06x %06x", px(219, 40), px(220, 40));

    /* 8. Scaled: the 64x40 picture into 32x20, linear: a smooth gradient still. */
    frame(&all, 0x000000);
    struct gpu_draw sc = { pic_id, { 0, 0, 64, 40 }, { 10, 150, 32, 20 }, 1, 255, 0, 0, { 0, 0, 0, 0 }, 0 };
    gc->draw(&sc);
    assert(gc->end() == 0);
    readback();
    CHECK(chan(px(10, 150), 16) < chan(px(41, 150), 16) && chan(px(10, 150), 8) < chan(px(10, 169), 8),
          "scaled gradient %06x %06x %06x", px(10, 150), px(41, 150), px(10, 169));

    /* 9. A partial upload: a 4x4 green patch changes; nothing else does. */
    for (int y = 8; y < 12; y++) for (int x = 8; x < 12; x++) pic[y * 64 + x] = 0xff00ff00u;
    struct gpu_rect patch = { 8, 8, 4, 4 };
    gc->image_update(pic_id, &patch);
    frame(&all, 0x000000);
    gc->draw(&d);
    assert(gc->end() == 0);
    readback();
    CHECK(px(109, 59) == 0x00ff00 && px(107, 59) == (pic[9 * 64 + 7] & 0xffffffu), "partial upload %06x %06x",
          px(109, 59), px(107, 59));

    /* 10. Damage confines drawing: a fill over everything changes only the
     * damage, a band off the top and left so mirrored rows would show. */
    struct gpu_rect dmg = { 30, 40, 50, 20 };
    assert(gc->begin(&dmg) == 0);
    gc->fill(&all, 0x123456);
    assert(gc->end() == 0);
    readback();
    CHECK(px(30, 40) == 0x123456 && px(79, 59) == 0x123456, "damage not drawn: %06x %06x", px(30, 40), px(79, 59));
    CHECK(px(29, 40) != 0x123456 && px(30, 39) != 0x123456 && px(80, 59) != 0x123456 && px(79, 60) != 0x123456 &&
          px(30, H - 41) != 0x123456, "drawn outside the damage: %06x %06x %06x %06x %06x",
          px(29, 40), px(30, 39), px(80, 59), px(79, 60), px(30, H - 41));

    gc->image_destroy(pic_id);
    gc->image_destroy(half_id);
    gc->image_destroy(white_id);
}

#endif
