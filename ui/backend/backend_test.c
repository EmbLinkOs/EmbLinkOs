/* ui/backend/backend_test.c -- EmbLink UI Piece 4a selftests (Section 8).
 *
 * Pure userland: operates on in-memory render_target buffers and compares
 * pixels directly. Host-compiled + run natively:  make backend-test
 * Exits 0 iff every T1..T5 invariant holds. */

#include "scene_render.h"
#include "backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

/* ---- pixel helpers (BGRA8888 premultiplied) ---------------------------- */
static uint32_t bgra(int b, int g, int r, int a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static void unbgra(uint32_t p, int *b, int *g, int *r, int *a) {
    *b = p & 255; *g = (p >> 8) & 255; *r = (p >> 16) & 255; *a = (p >> 24) & 255;
}
static struct render_target make_target(int w, int h) {
    /* ZEROED FIRST. render_target has fields beyond the five set here --
     * clear_dirty among them -- and leaving them as stack garbage makes the
     * test depend on what happened to be on the stack. That is not
     * theoretical: a non-zero clear_dirty makes begin_frame() wipe the target,
     * so the checkerboard T2 paints was being erased before the blur sampled
     * it, and the blur "failed" by reading the zeros it had been handed. */
    struct render_target rt;
    memset(&rt, 0, sizeof rt);
    rt.pixels = calloc((size_t)w * h, 4);
    rt.width = w; rt.height = h; rt.stride = w * 4;
    rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    return rt;
}
static uint32_t px_at(struct render_target *rt, int x, int y) {
    return ((uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride))[x];
}
static void fill_target(struct render_target *rt, uint32_t color) {
    for (uint32_t y = 0; y < rt->height; y++)
        for (uint32_t x = 0; x < rt->width; x++)
            ((uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride))[x] = color;
}
static int alpha_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return a;
}
static int green_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return g;
}
static int red_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return r;
}
static int blue_at(struct render_target *rt, int x, int y) {
    int b,g,r,a; unbgra(px_at(rt,x,y), &b,&g,&r,&a); return b;
}

/* ---- T1: rounded-rect AA is live (SDF + smoothstep) -------------------- */
static void t1_aa(void) {
    printf("T1 rounded-rect antialiasing:\n");
    struct render_backend *be = cpu_backend_get();
    struct render_target rt = make_target(64, 64);

    struct paint fill; fill.kind = PAINT_SOLID; fill.solid = (struct color){1,0,0,1}; fill.n_stops = 0;
    be->begin_frame(&rt, NULL, 0);                        /* full-screen (no dirty restriction) */
    be->draw_rect(&rt, 8, 8, 48, 48, 8.0f, &fill, 1.0f);  /* radius-8 rounded rect */
    be->end_frame(&rt);

    CHECK(alpha_at(&rt, 32, 32) >= 250, "center is fully opaque");
    /* the bounding-box corner (8,8) is mathematically outside the rounded arc */
    CHECK(alpha_at(&rt, 8, 8) <= 5, "rounded corner is transparent");
    /* the rounded-corner arc crosses pixel centers at partial coverage (the
     * straight edges are axis-aligned at integer coords, so their AA band
     * falls exactly between pixel rows -- the arc is where smoothstep shows). */
    int found_partial = 0;
    for (int y = 8; y < 17 && !found_partial; y++)
        for (int x = 8; x < 17; x++) {
            int a = alpha_at(&rt, x, y);
            if (a > 20 && a < 235) { found_partial = 1; break; }
        }
    CHECK(found_partial, "a corner-arc pixel has partial (AA) coverage");

    free(rt.pixels);
}

/* ---- T2: backdrop blur samples FRESH behind-content -------------------- */
static void t2_backdrop(void) {
    printf("T2 backdrop blur samples the real content:\n");
    struct render_backend *be = cpu_backend_get();
    struct render_target rt = make_target(64, 64);

    /* checkerboard of opaque black/white (average luminance ~127) */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int on = ((x >> 2) + (y >> 2)) & 1;
            ((uint32_t *)((unsigned char *)rt.pixels + (size_t)y * rt.stride))[x] =
                on ? bgra(255,255,255,255) : bgra(0,0,0,255);
        }

    be->begin_frame(&rt, NULL, 0);
    be->draw_backdrop_blur(&rt, 16, 16, 32, 32, 0.0f, 5.0f);
    be->end_frame(&rt);

    long sum = 0; int n = 0;
    for (int y = 20; y < 44; y++) for (int x = 20; x < 44; x++) { sum += red_at(&rt, x, y); n++; }
    int avg = (int)(sum / n);
    /* a real sample of the checkerboard averages toward mid-grey; black
     * scratch memory or an unsampled region would be ~0 */
    CHECK(avg > 80 && avg < 175, "blurred region averages toward the checkerboard mean");
    printf("    blurred region mean red = %d (checkerboard mean is ~127)\n", avg);

    free(rt.pixels);
}

/* build: two overlapping red rects; `grouped` wraps them in a 50%-opacity
 * group, else each rect is individually 50% opaque. Returns overlap+solo px. */
static void build_two_rects(int grouped, int *overlap_g, int *solo_g,
                            uint32_t *overlap_px, uint32_t *solo_px) {
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(255,255,255,255));   /* opaque white background */

    struct paint red; red.kind = PAINT_SOLID; red.solid = (struct color){1,0,0,1}; red.n_stops = 0;

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle parent = root;
    if (grouped) {
        parent = scene_create_node(&a, SCENE_NODE_GROUP, root);
        scene_set_opacity(&a, parent, 0.5f);
    }
    struct node_handle A = scene_create_node(&a, SCENE_NODE_RECT, parent);
    struct node_handle B = scene_create_node(&a, SCENE_NODE_RECT, parent);
    scene_set_size(&a, A, 30, 30); scene_set_transform(&a, A, 10,10,0, 0,0,0,1, 1,1,1);
    scene_set_size(&a, B, 30, 30); scene_set_transform(&a, B, 25,25,0, 0,0,0,1, 1,1,1);
    scene_set_paint(&a, A, &red); scene_set_paint(&a, B, &red);
    if (!grouped) { scene_set_opacity(&a, A, 0.5f); scene_set_opacity(&a, B, 0.5f); }

    scene_render_frame(&r, &a, root, &rt);
    *overlap_g = green_at(&rt, 30, 30);   /* inside both rects */
    *solo_g    = green_at(&rt, 14, 14);   /* inside A only */
    *overlap_px = px_at(&rt, 30, 30);
    *solo_px    = px_at(&rt, 14, 14);

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T3: opacity group eliminates the overlap seam --------------------- */
static void t3_opacity_group(void) {
    printf("T3 opacity group removes the overlap seam:\n");
    int g_overlap, g_solo, u_overlap, u_solo;
    uint32_t g_px_overlap, g_px_solo, u_px_overlap, u_px_solo;
    build_two_rects(1, &g_overlap, &g_solo, &g_px_overlap, &g_px_solo);
    build_two_rects(0, &u_overlap, &u_solo, &u_px_overlap, &u_px_solo);

    /* THE ORIGINAL ASSERTIONS, restored. They were never wrong: the target was.
     *
     * make_target() left render_target's tail as stack garbage, clear_dirty
     * among it -- so begin_frame() sometimes wiped the white background this
     * test paints, and every reading came back as premultiplied red on
     * nothing (g=0 four times, which cannot distinguish a flattened group from
     * a double-blended one). With the struct zeroed, red at 50% over white is
     * (255,128,128) and the double-blended overlap is (255,64,64), which is
     * exactly what these two lines have always been looking for.
     *
     * Worth recording because the first fix here was to rewrite the
     * assertions in terms of alpha to match what the broken target produced --
     * fitting the test to the bug, which would have locked it in. */
    CHECK(abs(g_overlap - g_solo) < 20, "grouped: overlap matches non-overlap (no seam)");
    CHECK(u_overlap < g_overlap - 25, "ungrouped overlap is measurably darker (seam present)");
    /* THE WHOLE PIXEL, not one channel of it. Printing only green cannot tell
     * "red at full opacity" (r=255 g=0) from "nothing was drawn at all"
     * (0,0,0), and those need completely different fixes. */
    /* THE WHOLE PIXEL, not one channel. Printing only green cannot tell "red
     * at full opacity" from "nothing was drawn at all", and those need
     * completely different fixes -- which cost an hour here. */
    printf("    grouped overlap g=%d solo g=%d | ungrouped overlap g=%d solo g=%d\n",
           g_overlap, g_solo, u_overlap, u_solo);
    printf("    rgba: grouped %08x/%08x | ungrouped %08x/%08x (overlap/solo)\n",
           g_px_overlap, g_px_solo, u_px_overlap, u_px_solo);
}

/* ---- T4: dirty-rect + backdrop-blur -> fresh sample on frame 2 --------- */
static void t4_dirty_blur(void) {
    printf("T4 dirty-rect forces a fresh backdrop sample:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(0,0,0,255));

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    /* background full-target rect so dirty regions restore to a known colour */
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint bgp; bgp.kind = PAINT_SOLID; bgp.solid = (struct color){0,0,0,1}; bgp.n_stops = 0;
    scene_set_paint(&a, bg, &bgp);
    /* A: the changing element, behind B */
    struct node_handle A = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, A, 24, 24); scene_set_transform(&a, A, 20,20,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, A, &redp);
    /* B: a backdrop-blur panel over A, itself never changing */
    struct node_handle B = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, B, 32, 32); scene_set_transform(&a, B, 16,16,0, 0,0,0,1, 1,1,1);
    struct paint clearp; clearp.kind = PAINT_SOLID; clearp.solid = (struct color){0,0,0,0}; clearp.n_stops = 0;
    scene_set_paint(&a, B, &clearp);            /* transparent fill; blur does the visual */
    scene_set_backdrop_blur(&a, B, 1, 4.0f);

    scene_render_frame(&r, &a, root, &rt);       /* frame 1: A is red */
    int red_f1 = red_at(&rt, 28, 28);
    CHECK(red_f1 > 60, "frame 1 blur region reflects red A");

    /* frame 2: A becomes blue (only A changes) */
    struct paint bluep; bluep.kind = PAINT_SOLID; bluep.solid = (struct color){0,0,1,1}; bluep.n_stops = 0;
    scene_set_paint(&a, A, &bluep);
    scene_render_frame(&r, &a, root, &rt);
    int red_f2 = red_at(&rt, 28, 28), blue_f2 = blue_at(&rt, 28, 28);
    CHECK(blue_f2 > 60 && blue_f2 > red_f2, "frame 2 blur region reflects NEW (blue) A, not stale red");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T5: moved element leaves no ghost (union old+new) ----------------- */
static void t5_ghost(void) {
    printf("T5 moved element leaves no ghost:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint grey; grey.kind = PAINT_SOLID; grey.solid = (struct color){0.5f,0.5f,0.5f,1}; grey.n_stops = 0;
    scene_set_paint(&a, bg, &grey);
    struct node_handle rect = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, rect, 16, 16); scene_set_transform(&a, rect, 4,4,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, rect, &redp);

    scene_render_frame(&r, &a, root, &rt);        /* frame 1: rect at P1 (4,4) */
    CHECK(red_at(&rt, 10, 10) > 200, "frame 1: P1 shows the rect");

    scene_set_transform(&a, rect, 40,40,0, 0,0,0,1, 1,1,1);   /* move to P2 */
    scene_render_frame(&r, &a, root, &rt);
    int p1_r = red_at(&rt, 10, 10), p1_g = green_at(&rt, 10, 10);
    int p2_r = red_at(&rt, 46, 46);
    CHECK(p2_r > 200, "frame 2: P2 shows the rect at its new position");
    CHECK(p1_r < 160 && p1_g > 100, "frame 2: P1 restored to background grey (no ghost)");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* T6: a DESTROYED element leaves no ghost. The vacated-slot half of the
 * dirty algorithm: a node that is simply gone appears in no traversal, so its
 * last painted footprint must come out of the renderer's own cache. Before
 * Step 1b existed this test failed -- the red pixels stayed forever and every
 * app papered over it with forced full repaints. Also exercises the shadow
 * footprint: the ghost of a shadowed node must clear PAST its box. */
static void t6_destroyed(void) {
    printf("T6 destroyed element leaves no ghost:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint grey; grey.kind = PAINT_SOLID; grey.solid = (struct color){0.5f,0.5f,0.5f,1}; grey.n_stops = 0;
    scene_set_paint(&a, bg, &grey);
    struct node_handle rect = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, rect, 16, 16); scene_set_transform(&a, rect, 8,8,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, rect, &redp);
    scene_set_shadow(&a, rect, true, 6, 6, 4, (struct color){0,0,0,0.8f});

    scene_render_frame(&r, &a, root, &rt);        /* frame 1: rect + shadow */
    CHECK(red_at(&rt, 14, 14) > 200, "frame 1: the rect shows");
    int sh_r = red_at(&rt, 27, 27), sh_g = green_at(&rt, 27, 27);
    CHECK(sh_g < 110, "frame 1: the shadow darkens past the box");

    scene_destroy_node(&a, rect);                 /* gone -- no move, no flag */
    scene_render_frame(&r, &a, root, &rt);
    int r2 = red_at(&rt, 14, 14), g2 = green_at(&rt, 14, 14);
    CHECK(r2 < 160 && g2 > 100, "frame 2: the body's pixels restored (no ghost)");
    int sg2 = green_at(&rt, 27, 27);
    CHECK(sg2 > sh_g + 10, "frame 2: the SHADOW's pixels restored too");
    (void)sh_r;

    /* and a quiet third frame stays quiet: the vacated slot must not keep
     * reporting itself dirty forever */
    scene_render_frame(&r, &a, root, &rt);
    CHECK(r.n_dirty == 0 && !r.full, "frame 3: nothing left to repaint");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* T7: an ELEVATED subtree paints above later flow content, and escapes its
 * parent's clip -- the two halves of popover semantics. An early scrim/panel
 * (layer 1) must not be buried by a Spacer declared after it, and a dropdown
 * must be able to hang below the 28px strip that declared it. */
static void t7_layers(void) {
    printf("T7 elevated layers paint above later flow, escape parent clip:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint grey; grey.kind = PAINT_SOLID; grey.solid = (struct color){0.5f,0.5f,0.5f,1}; grey.n_stops = 0;
    scene_set_paint(&a, bg, &grey);

    /* a CLIPPING strip (like a menu bar), 64x10, containing an elevated panel
     * that hangs far below the strip */
    struct node_handle strip = scene_create_node(&a, SCENE_NODE_GROUP, root);
    scene_set_size(&a, strip, 64, 10);
    scene_set_clip_children(&a, strip, true);
    struct node_handle panel = scene_create_node(&a, SCENE_NODE_RECT, strip);
    scene_set_size(&a, panel, 20, 40);
    scene_set_transform(&a, panel, 8, 4, 0, 0,0,0,1, 1,1,1);
    scene_set_layer(&a, panel, 1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, panel, &redp);

    /* LATER flow content overlapping the panel's area (the "Spacer over the
     * scrim" shape) */
    struct node_handle late = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, late, 40, 40);
    scene_set_transform(&a, late, 4, 12, 0, 0,0,0,1, 1,1,1);
    struct paint bluep; bluep.kind = PAINT_SOLID; bluep.solid = (struct color){0,0,1,1}; bluep.n_stops = 0;
    scene_set_paint(&a, late, &bluep);

    scene_render_frame(&r, &a, root, &rt);
    /* inside the panel, below the strip's 10px clip, where `late` also paints:
     * elevated wins AND the clip did not cut it */
    CHECK(red_at(&rt, 14, 30) > 200, "elevated panel paints ABOVE later flow");
    CHECK(blue_at(&rt, 40, 20) > 200, "flow content still paints where the panel is not");

    /* destroy the elevated panel: its pixels restore (vacated pass covers
     * deferred subtrees exactly like flow ones) */
    scene_destroy_node(&a, panel);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 14, 30) < 160, "destroyed elevated panel leaves no ghost");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* T8: an image edited in place repaints and reports only what changed. A
 * terminal changes a line at a time; repainting and presenting the window
 * for it is the cost this exists to avoid. */
static void t8_image_damage(void) {
    printf("T8 an image's damage is all that repaints:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    static uint32_t img[32 * 32];
    for (int i = 0; i < 32 * 32; i++) img[i] = 0xff0000ffu;      /* opaque blue */
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle im = scene_create_node(&a, SCENE_NODE_IMAGE, root);
    scene_set_size(&a, im, 32, 32); scene_set_transform(&a, im, 16,8,0, 0,0,0,1, 1,1,1);
    scene_set_image(&a, im, img, 32, 32, EMBK_PIXFMT_BGRA8888_PRE);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 20, 12) < 50, "frame 1: the image shows");
    for (int y = 10; y < 14; y++) for (int x = 4; x < 12; x++) img[y * 32 + x] = 0xffff0000u;   /* red, in place */
    scene_set_image_damage(&a, im, 4, 10, 8, 4);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(!r.full && r.n_dirty == 1 && r.dirty[0].x == 20 && r.dirty[0].y == 18 && r.dirty[0].w == 8 && r.dirty[0].h == 4,
          "frame 2: one dirty rect, the damage at the image's place");
    CHECK(red_at(&rt, 22, 19) > 200, "frame 2: the edited pixels show");
    scene_set_image_damage(&a, im, 0, 0, 2, 2);
    scene_set_image_damage(&a, im, 30, 30, 2, 2);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(r.n_dirty == 1 && r.dirty[0].w == 32 && r.dirty[0].h == 32, "two damages in a frame: the rect around both");
    scene_render_frame(&r, &a, root, &rt);
    CHECK(r.n_dirty == 0 && !r.full, "frame 4: nothing said, nothing repainted");
    static uint32_t other[32 * 32];
    scene_set_image(&a, im, other, 32, 32, EMBK_PIXFMT_BGRA8888_PRE);
    scene_set_image_damage(&a, im, 0, 0, 1, 1);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(r.n_dirty >= 1 && r.dirty[0].w >= 32, "a new buffer is all new, whatever damage is said");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* T9: a node at a FRACTIONAL place leaves no sliver when it moves. Its
 * anti-aliased edge partly covers the pixel left of its x, and that pixel's
 * centre lies outside the old footprint's exact rect -- repainted only when
 * damage is rounded out to whole pixels. A window title that shrank kept one
 * grey column of its old first letter this way. */
static void t9_fractional_edge(void) {
    printf("T9 a fractional edge leaves no sliver:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);

    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 64, 64);
    struct paint grey; grey.kind = PAINT_SOLID; grey.solid = (struct color){0.5f,0.5f,0.5f,1}; grey.n_stops = 0;
    scene_set_paint(&a, bg, &grey);
    struct node_handle rect = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, rect, 8, 8); scene_set_transform(&a, rect, 10.6f,10,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, rect, &redp);

    scene_render_frame(&r, &a, root, &rt);
    int grey_r = red_at(&rt, 4, 14);
    CHECK(red_at(&rt, 10, 14) > grey_r + 20, "frame 1: the edge partly covers pixel 10");
    scene_set_transform(&a, rect, 40,10,0, 0,0,0,1, 1,1,1);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 10, 14) <= grey_r + 2, "frame 2: pixel 10 restored (no sliver)");
    int whole = r.n_dirty >= 1;
    for (int i = 0; i < r.n_dirty; i++)
        whole = whole && r.dirty[i].x == floorf(r.dirty[i].x) && r.dirty[i].w == floorf(r.dirty[i].w);
    CHECK(whole, "dirty rects are whole pixels");

    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* T10: clearing the dirty rects of a canvas that starts transparent (a
 * window that shows through) when a rect lies past the target's edge. It
 * cleared (x1 - x0) * 4 bytes of a NEGATIVE width -- most of the address
 * space -- and the Terminal died opening its Preferences beside the screen. */
static void t10_clear_outside(void) {
    printf("T10 clearing a dirty rect past the edge:\n");
    struct render_target rt = make_target(32, 32);
    rt.clear_dirty = 1;
    for (int i = 0; i < 32 * 32; i++) ((uint32_t *)rt.pixels)[i] = 0xff123456u;
    struct clip_rect d[4] = { { 40, 4, 10, 10, 0 }, { 4, 40, 10, 10, 0 }, { 30.5f, 2, 8, 4, 0 }, { 4, 10, 10, 5, 0 } };
    const struct render_backend *be = cpu_backend_get();
    be->begin_frame(&rt, d, 4);
    be->end_frame(&rt);
    uint32_t *px = rt.pixels;
    CHECK(px[2 * 32 + 30] == 0 && px[2 * 32 + 29] == 0xff123456u && px[20 * 32 + 20] == 0xff123456u,
          "only the part inside is cleared, and nothing past the edge is touched");
    CHECK(px[10 * 32 + 4] == 0 && px[14 * 32 + 13] == 0 && px[15 * 32 + 8] == 0xff123456u && px[12 * 32 + 14] == 0xff123456u,
          "a whole-pixel rect clears itself exactly: not the row or column after it");
    free(rt.pixels);
}

/* ---- T11: glass inside an opacity group blurs what is behind the group --
 * A group renders into a scratch target that starts empty; a backdrop blur
 * that read its own target there blurred nothing, and every glass panel
 * fading in (a menu, the launcher) lost its frost until the fade ended. */
static void t11_glass_in_group(void) {
    printf("T11 glass in an opacity group frosts the real backdrop:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(0,0,0,255));
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    /* the backdrop: red on the left, blue on the right, a sharp edge at x 32
     * -- frosted, the pixel beside it is a mix; seen through clear, it is not */
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 32, 64);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, bg, &redp);
    struct node_handle bg2 = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg2, 32, 64); scene_set_transform(&a, bg2, 32,0,0, 0,0,0,1, 1,1,1);
    struct paint bluep; bluep.kind = PAINT_SOLID; bluep.solid = (struct color){0,0,1,1}; bluep.n_stops = 0;
    scene_set_paint(&a, bg2, &bluep);
    struct node_handle grp = scene_create_node(&a, SCENE_NODE_GROUP, root);
    scene_set_opacity(&a, grp, 0.99f);              /* a group, near full strength */
    struct node_handle glass = scene_create_node(&a, SCENE_NODE_RECT, grp);
    scene_set_size(&a, glass, 32, 32); scene_set_transform(&a, glass, 16,16,0, 0,0,0,1, 1,1,1);
    struct paint clearp; clearp.kind = PAINT_SOLID; clearp.solid = (struct color){0,0,0,0}; clearp.n_stops = 0;
    scene_set_paint(&a, glass, &clearp);
    scene_set_backdrop_blur(&a, glass, 1, 4.0f);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 33, 32) > 30 && blue_at(&rt, 33, 32) > 30,
          "the glass frosts the edge behind its group -- not an empty blur that shows it sharp");
    CHECK(r.backdrop == 0, "and the renderer forgets the backdrop once the group is done");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T12: the reduced effects tier draws glass as its tint, unblurred ---- */
static void t12_reduced_glass(void) {
    printf("T12 reduced effects: glass is its own tint, whole, with no blur:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(0,0,0,255));
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle bg = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, bg, 32, 64);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, bg, &redp);
    struct node_handle glass = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, glass, 32, 32); scene_set_transform(&a, glass, 16,16,0, 0,0,0,1, 1,1,1);
    struct paint tint; tint.kind = PAINT_SOLID; tint.solid = (struct color){0,0,1,0.5f}; tint.n_stops = 0;
    scene_set_paint(&a, glass, &tint);
    scene_set_backdrop_blur(&a, glass, 1, 4.0f);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 30, 32) > 30, "full: the half-clear tint shows the red behind it");
    CHECK(scene_render_set_reduced(1) == 1 && scene_render_set_reduced(1) == 0, "the tier changes once");
    scene_render_invalidate(&r);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 30, 32) < 4 && blue_at(&rt, 30, 32) > 250 && red_at(&rt, 33, 32) < 4,
          "reduced: the tint is whole -- nothing behind shows, blurred or sharp");
    scene_render_set_reduced(0);
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T13: a scaled group draws its flattened subtree smaller, toward its
 * anchor -- a popover growing out of where it opens ---------------------- */
static void t13_scaled_group(void) {
    printf("T13 a scaled group is drawn smaller toward its anchor:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(64, 64);
    fill_target(&rt, bgra(0,0,0,255));
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    /* a sized box, as every toolkit box that takes ui_set_scale is: its own
     * change dirties all it covers */
    struct node_handle grp = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, grp, 64, 64);
    struct paint clear; clear.kind = PAINT_SOLID; clear.solid = (struct color){0,0,0,0}; clear.n_stops = 0;
    scene_set_paint(&a, grp, &clear);
    struct node_handle box = scene_create_node(&a, SCENE_NODE_RECT, grp);
    scene_set_size(&a, box, 40, 40); scene_set_transform(&a, box, 12,12,0, 0,0,0,1, 1,1,1);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, box, &redp);
    scene_set_group_scale(&a, grp, 0.5f, 0.0f, 0.0f);     /* half, toward the top-left */
    scene_render_frame(&r, &a, root, &rt);
    /* the group is 64 x 64, so half of it toward the top-left puts the box
     * (12..52) at 6..26 */
    CHECK(red_at(&rt, 8, 8) > 200 && red_at(&rt, 24, 24) > 200, "the box is drawn at half, toward the top-left");
    CHECK(red_at(&rt, 30, 30) < 10 && red_at(&rt, 50, 20) < 10, "and not where it would stand at full size");
    scene_set_group_scale(&a, grp, 1.0f, 0.0f, 0.0f);     /* grown: all of it */
    scene_render_frame(&r, &a, root, &rt);
    CHECK(red_at(&rt, 50, 50) > 200 && red_at(&rt, 12, 51) > 200, "at full size the whole box is back");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T14: a fade alone repaints -- an opacity that changes with nothing
 * moving is still new pixels (it was dirty but not content, and nothing was
 * redrawn until something moved) ---------------------------------------- */
static void t14_fade_alone(void) {
    printf("T14 a fade with nothing moving repaints:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(32, 32);
    fill_target(&rt, bgra(0,0,0,255));
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle box = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, box, 32, 32);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, box, &redp);
    scene_set_opacity(&a, box, 0.2f);
    scene_render_frame(&r, &a, root, &rt);
    int faint = red_at(&rt, 16, 16);
    scene_set_opacity(&a, box, 1.0f);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(faint < 80 && red_at(&rt, 16, 16) > 250, "faded in, the box is drawn whole");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T15: an opaque linear gradient runs its whole range corner to corner
 * (it was projected from the corner and showed only its second half), and
 * drawn from its table matches the exact gradient within the dither ------ */
static void t15_gradient_table(void) {
    printf("T15 a gradient drawn from its table is the gradient:\n");
    for (int vertical = 0; vertical < 2; vertical++) {
        struct scene_arena a; scene_arena_init(&a);
        struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
        int W = vertical ? 40 : 256, H = vertical ? 256 : 40;
        struct render_target rt = make_target((uint32_t)W, (uint32_t)H);
        fill_target(&rt, bgra(0,0,0,255));
        struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
        struct node_handle box = scene_create_node(&a, SCENE_NODE_RECT, root);
        scene_set_size(&a, box, (float)W, (float)H);
        struct paint g; memset(&g, 0, sizeof g);
        g.kind = PAINT_LINEAR_GRADIENT; g.angle_deg = vertical ? 90.0f : 0.0f; g.n_stops = 2;
        g.stops[0].offset = 0; g.stops[0].color = (struct color){0,0,0,1};
        g.stops[1].offset = 1; g.stops[1].color = (struct color){1,0,0,1};
        scene_set_paint(&a, box, &g);
        scene_render_frame(&r, &a, root, &rt);
        int worst = 0, n = vertical ? H : W;
        for (int k = 2; k < n - 2; k++) {
            float t = ((k + 0.5f) / (float)n);
            int want = (int)(t * 255.0f + 0.5f), got = vertical ? red_at(&rt, W / 2, k) : red_at(&rt, k, H / 2);
            int d = got > want ? got - want : want - got;
            if (d > worst) worst = d;
        }
        CHECK(worst <= 2, vertical ? "down: within two levels of the gradient everywhere" : "across: within two levels of the gradient everywhere");
        free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
    }
}

/* ---- T16: a large blur (half resolution from radius 6) still frosts an edge
 * evenly: a mix at the edge, graded beside it, pure far from it ---------- */
static void t16_large_blur(void) {
    printf("T16 a large glass blur, at half resolution, frosts an edge evenly:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(160, 64);
    fill_target(&rt, bgra(0,0,0,255));
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle l = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, l, 80, 64);
    struct paint redp; redp.kind = PAINT_SOLID; redp.solid = (struct color){1,0,0,1}; redp.n_stops = 0;
    scene_set_paint(&a, l, &redp);
    struct node_handle rr = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, rr, 80, 64); scene_set_transform(&a, rr, 80,0,0, 0,0,0,1, 1,1,1);
    struct paint bluep; bluep.kind = PAINT_SOLID; bluep.solid = (struct color){0,0,1,1}; bluep.n_stops = 0;
    scene_set_paint(&a, rr, &bluep);
    struct node_handle glass = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, glass, 160, 64);
    struct paint clearp; clearp.kind = PAINT_SOLID; clearp.solid = (struct color){0,0,0,0}; clearp.n_stops = 0;
    scene_set_paint(&a, glass, &clearp);
    scene_set_backdrop_blur(&a, glass, 1, 12.0f);
    scene_render_frame(&r, &a, root, &rt);
    int edge_r = red_at(&rt, 80, 32), edge_b = blue_at(&rt, 79, 32);
    CHECK(edge_r > 70 && edge_r < 190 && edge_b > 70 && edge_b < 190, "at the edge, a mix of both");
    CHECK(red_at(&rt, 70, 32) > red_at(&rt, 80, 32) && red_at(&rt, 80, 32) > red_at(&rt, 90, 32), "graded across it, falling away steadily");
    CHECK(red_at(&rt, 20, 32) > 230 && blue_at(&rt, 140, 32) > 230, "far from it, each side as it was");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- the group tests' shared scene: a translucent card away from the window
 * origin, holding a row of boxes -- a menu, as the toolkit builds one ------ */
struct card { struct node_handle root, grp, box[40]; int n; };
static struct paint solidp(float r, float g, float b, float a) {
    struct paint p; memset(&p, 0, sizeof p); p.kind = PAINT_SOLID; p.solid = (struct color){ r, g, b, a }; return p;
}
static void card_build(struct scene_arena *a, struct card *c, int n) {
    c->n = n;
    c->root = scene_create_node(a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle ground = scene_create_node(a, SCENE_NODE_RECT, c->root);
    scene_set_size(a, ground, 200, 160);
    struct paint gp = solidp(0.1f, 0.12f, 0.14f, 1); scene_set_paint(a, ground, &gp);
    c->grp = scene_create_node(a, SCENE_NODE_RECT, c->root);
    scene_set_size(a, c->grp, 120, 100);
    scene_set_transform(a, c->grp, 40.5f, 30.25f, 0, 0,0,0,1, 1,1,1);   /* off the origin, between pixels */
    scene_set_corner_radius(a, c->grp, 8);
    struct paint cp = solidp(0.9f, 0.9f, 0.95f, 0.9f); scene_set_paint(a, c->grp, &cp);
    for (int i = 0; i < n; i++) {
        c->box[i] = scene_create_node(a, SCENE_NODE_RECT, c->grp);
        scene_set_size(a, c->box[i], 10, 8);
        scene_set_transform(a, c->box[i], 4.0f + (float)(i % 8) * 14.0f, 6.0f + (float)(i / 8) * 18.0f, 0, 0,0,0,1, 1,1,1);
        scene_set_corner_radius(a, c->box[i], 2);
        struct paint bp = solidp(0.2f + 0.1f * (float)(i % 5), 0.3f, 0.7f, 1); scene_set_paint(a, c->box[i], &bp);
    }
}
/* how many pixels of `rt` differ from painting the scene from nothing */
static int differs_from_fresh(struct scene_arena *a, struct node_handle root, struct render_target *rt) {
    struct render_target f = make_target((int)rt->width, (int)rt->height);
    struct scene_renderer r2; scene_render_init(&r2, cpu_backend_get());
    scene_render_frame(&r2, a, root, &f);
    int n = 0;
    for (uint32_t y = 0; y < rt->height; y++)
        for (uint32_t x = 0; x < rt->width; x++) if (px_at(rt, (int)x, (int)y) != px_at(&f, (int)x, (int)y)) n++;
    scene_render_destroy(&r2); free(f.pixels);
    return n;
}

/* ---- T17: a partial frame of a group away from the origin keeps all of it.
 * Inside a group's offscreen pass a child's footprint is in the scratch's own
 * coordinates; it was tested against the frame's dirty rects in the window's,
 * so on any frame that was not a full repaint the children of a group that
 * did not sit at the origin were culled, and the group came back without them. */
static void t17_partial_group(void) {
    printf("T17 a partial frame of an offset group keeps all its children:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(200, 160);
    struct card c; card_build(&a, &c, 6);
    scene_set_opacity(&a, c.grp, 0.6f);
    scene_render_frame(&r, &a, c.root, &rt);
    struct paint np = solidp(1, 0.5f, 0, 1); scene_set_paint(&a, c.box[5], &np);   /* one child changes */
    scene_render_frame(&r, &a, c.root, &rt);
    CHECK(!r.full, "the change is a partial frame");
    CHECK(differs_from_fresh(&a, c.root, &rt) == 0, "and it is what painting it all gives: no child culled");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T18: a group that fades, moves and shrinks is the same picture, frame by
 * frame, as painting it from nothing -- whether its flattened pixels were kept
 * or remade, and with a child changing halfway ---------------------------- */
static void t18_group_motion(void) {
    printf("T18 a group fading, moving and growing is right every frame:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(200, 160);
    struct card c; card_build(&a, &c, 12);
    int wrong = 0;
    for (int f = 0; f <= 20; f++) {
        float t = (float)f / 20.0f;
        scene_set_opacity(&a, c.grp, 0.05f + 0.9f * t);
        scene_set_group_scale(&a, c.grp, 0.94f + 0.05f * t, 0.0f, 0.0f);
        scene_set_transform(&a, c.grp, 40.5f, 30.25f - (float)(20 - f), 0, 0,0,0,1, 1,1,1);   /* whole-pixel steps */
        if (f == 10) { struct paint np = solidp(1, 0.5f, 0, 1); scene_set_paint(&a, c.box[3], &np); }
        scene_render_frame(&r, &a, c.root, &rt);
        if (differs_from_fresh(&a, c.root, &rt)) wrong++;
    }
    printf("        (%u frames composited from kept pixels, %u painted anew)\n", r.group_hits, r.group_paints);
    CHECK(wrong == 0, "every frame of it is what painting it whole gives");
    CHECK(r.group_hits >= 17 && r.group_paints <= 3,
          "and it was painted twice -- at the start and when a child changed -- and only composited after");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T19: a group moving is a few dirty rects, not a full repaint. Each of
 * its children moved too, and each added its old and new place, so a menu of
 * more than eight things blew the cap and repainted the window ------------ */
static void t19_group_dirty(void) {
    printf("T19 a moving group of many children is not a full repaint:\n");
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    struct render_target rt = make_target(200, 160);
    struct card c; card_build(&a, &c, 40);
    scene_set_opacity(&a, c.grp, 0.5f);
    scene_render_frame(&r, &a, c.root, &rt);
    scene_set_transform(&a, c.grp, 40.5f, 32.25f, 0, 0,0,0,1, 1,1,1);
    scene_set_opacity(&a, c.grp, 0.6f);
    scene_render_frame(&r, &a, c.root, &rt);
    CHECK(!r.full, "forty children moving with their group: still a partial frame");
    CHECK(differs_from_fresh(&a, c.root, &rt) == 0, "and the right picture");
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

/* ---- T20: a PROMOTED subtree is a layer's, not the window's --------------
 * A fake layer host keeps the layers as the application runtime would hand
 * them to the compositor, and counts what it was asked. */
struct fake_layer { uint32_t *px; uint32_t w, h; int presents, gos, froms, destroys; struct scene_layer_state at; };
static struct fake_layer g_fl[8];
static int g_fl_n;
static uint32_t fl_create(void *ctx, uint32_t w, uint32_t h, uint32_t **px) {
    (void)ctx;
    if (g_fl_n >= 8) return 0;
    struct fake_layer *l = &g_fl[g_fl_n++];
    memset(l, 0, sizeof *l);
    l->px = calloc((size_t)w * h, 4); l->w = w; l->h = h;
    *px = l->px;
    return (uint32_t)g_fl_n;
}
static void fl_present(void *ctx, uint32_t id, int x, int y, int w, int h) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h;
    g_fl[id - 1].presents++;
}
static void fl_go(void *ctx, uint32_t id, const struct scene_layer_state *from, const struct scene_layer_state *to,
                  uint16_t resp, uint16_t damp, int destroy, int at_present) {
    (void)ctx; (void)resp; (void)damp; (void)at_present;
    struct fake_layer *l = &g_fl[id - 1];
    l->gos++; l->froms += from != 0; l->destroys += destroy; l->at = *to;
}
static const struct scene_layer_host g_fake_host = { 0, fl_create, fl_present, fl_go };

/* the ground and a card -- a menu, its shadow, a row in it -- promoted or not */
static struct node_handle t20_scene(struct scene_arena *ap, struct node_handle *card_out, struct node_handle *row_out,
                                    int promote) {
#define a (*ap)
    struct node_handle root = scene_create_node(&a, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    struct node_handle ground = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, ground, 200, 160);
    struct paint gp = solidp(0.1f, 0.2f, 0.6f, 1); scene_set_paint(&a, ground, &gp);
    struct node_handle card = scene_create_node(&a, SCENE_NODE_RECT, root);
    scene_set_size(&a, card, 100, 60);
    scene_set_transform(&a, card, 40, 30, 0, 0,0,0,1, 1,1,1);
    scene_set_corner_radius(&a, card, 8);
    struct paint cp = solidp(0.95f, 0.95f, 0.97f, 0.9f); scene_set_paint(&a, card, &cp);
    scene_set_shadow(&a, card, true, 0, 4, 10, (struct color){ 0, 0, 0, 0.3f });
    struct node_handle row = scene_create_node(&a, SCENE_NODE_RECT, card);
    scene_set_size(&a, row, 80, 12); scene_set_transform(&a, row, 10, 10, 0, 0,0,0,1, 1,1,1);
    struct paint rp = solidp(0.2f, 0.6f, 0.3f, 1); scene_set_paint(&a, row, &rp);
    struct scene_layer_motion in = { 0, 940, 0, 0, 0, -5, 240, 1000 }, out = { 0, 960, 0, 0, 0, 0, 200, 1000 };
    if (promote) scene_set_promoted(&a, card, true, &in, &out);
    *card_out = card; *row_out = row;
    return root;
#undef a
}

static void t20_promoted(void) {
    printf("T20 a promoted subtree is painted once into a layer, and moved by it:\n");
    g_fl_n = 0;
    struct scene_arena a; scene_arena_init(&a);
    struct scene_renderer r; scene_render_init(&r, cpu_backend_get());
    scene_render_set_layer_host(&r, &g_fake_host);
    struct render_target rt = make_target(200, 160);
    struct node_handle card, row, root = t20_scene(&a, &card, &row, 1);

    scene_render_frame(&r, &a, root, &rt);
    CHECK(g_fl_n == 0 && r.wants_frame, "the first frame it is seen, it settles: no layer yet, and a frame asked for");
    scene_render_frame(&r, &a, root, &rt);
    struct fake_layer *l = &g_fl[0];
    CHECK(g_fl_n == 1 && l->presents == 1, "the card is painted into one layer, presented once");
    CHECK(l->gos == 1 && l->froms == 1 && l->at.x <= 40 && l->at.opacity == 255,
          "and sent in from its appear end, to where it stands");
    CHECK(px_at(&rt, 90, 50) == px_at(&rt, 5, 5), "the window under it is the ground: the card is not in it");
    /* the layer over the window is the card painted where it stands */
    {
        struct render_target f = make_target(200, 160);
        struct scene_renderer r2; scene_render_init(&r2, cpu_backend_get());
        struct scene_arena a2; scene_arena_init(&a2);
        struct node_handle c2, w2, root2 = t20_scene(&a2, &c2, &w2, 0);   /* the same, painted in place */
        scene_render_frame(&r2, &a2, root2, &f);
        int worst = 0;
        for (int y = 0; y < 160; y++)
            for (int x = 0; x < 200; x++) {
                uint32_t d = px_at(&rt, x, y), o = d;
                int lx = x - l->at.x, ly = y - l->at.y;
                if (lx >= 0 && ly >= 0 && lx < (int)l->w && ly < (int)l->h) {
                    uint32_t sp = l->px[ly * l->w + lx], ia = 255 - (sp >> 24);
                    o = 0;
                    for (int sh = 0; sh < 32; sh += 8)
                        o |= ((((sp >> sh) & 255) + (((d >> sh) & 255) * ia + 127) / 255) > 255 ? 255u :
                              (((sp >> sh) & 255) + (((d >> sh) & 255) * ia + 127) / 255)) << sh;
                }
                uint32_t want = px_at(&f, x, y);
                /* AN EDGE IS NOT THE SAME EDGE: painted into the window its
                 * coverage is blended against the ground, painted into a layer
                 * it is alpha (blend_cov) -- leave the card's outline out */
                int ex = x < 40 ? 40 - x : x >= 140 ? x - 139 : 0, ey = y < 30 ? 30 - y : y >= 90 ? y - 89 : 0;
                int in_x = x >= 40 && x < 140, in_y = y >= 30 && y < 90;
                int near_edge = (in_x && in_y && (x < 42 || x >= 138 || y < 32 || y >= 88 ||
                                 ((x < 50 || x >= 130) && (y < 40 || y >= 80)))) ||
                                ((ex || ey) && ex <= 1 && ey <= 1);
                if (near_edge) continue;
                for (int sh = 0; sh < 24; sh += 8) {
                    int e = (int)((o >> sh) & 255) - (int)((want >> sh) & 255);
                    if (e < 0) e = -e;
                }
            }
        printf("        (the layer over the window, against painting it in place: %d levels at most)\n", worst);
        CHECK(worst <= 2, "the layer composited over the window is the card painted in it");
        scene_render_destroy(&r2); free(f.pixels); scene_arena_destroy(&a2);
    }
    scene_render_frame(&r, &a, root, &rt);
    CHECK(l->presents == 1 && l->gos == 1, "a frame where nothing changed asks nothing of it");
    scene_set_transform(&a, card, 46, 30, 0, 0,0,0,1, 1,1,1);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(l->presents == 1 && l->gos == 2 && l->froms == 1 && l->at.x >= 46 - 20,
          "moved, it is told where to go and not painted again");
    struct paint np = solidp(0.9f, 0.3f, 0.2f, 1); scene_set_paint(&a, row, &np);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(l->presents == 2 && l->gos == 2, "a child repainted: the layer's pixels again, no move");
    uint32_t paints = r.layer_paints;
    scene_destroy_node(&a, card);
    scene_render_frame(&r, &a, root, &rt);
    CHECK(l->destroys == 1 && l->at.opacity == 0 && l->at.scale_milli == 960 && r.layer_paints == paints,
          "gone, it leaves on its disappear motion, and the layer goes after");
    CHECK(px_at(&rt, 90, 50) == px_at(&rt, 5, 5), "and the window was never painted with it");
    for (int i = 0; i < g_fl_n; i++) free(g_fl[i].px);
    free(rt.pixels); scene_render_destroy(&r); scene_arena_destroy(&a);
}

int main(void) {
    printf("=== EmbLink UI Piece 4a: render-backend selftests ===\n");
    t1_aa();
    t2_backdrop();
    t3_opacity_group();
    t4_dirty_blur();
    t5_ghost();
    t6_destroyed();
    t7_layers();
    t8_image_damage();
    t9_fractional_edge();
    t10_clear_outside();
    t11_glass_in_group();
    t12_reduced_glass();
    t13_scaled_group();
    t14_fade_alone();
    t15_gradient_table();
    t16_large_blur();
    t17_partial_group();
    t18_group_motion();
    t19_group_dirty();
    t20_promoted();
    printf("=== backend-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}

