/* ui/backend/font_test.c -- EmbLink UI Piece 4b selftests (Section 7).
 *
 * Pure userland. Hand-constructs a minimal valid .ttf in-memory (a table
 * directory + head/maxp/hhea/hmtx/cmap(fmt4)/loca/glyf with a square glyph, a
 * curved glyph, and a composite glyph) so the parser/rasterizer is tested
 * against KNOWN expected output, with no dependency on any external font.
 *   make font-test   -> exits 0 iff every T1..T5 invariant holds. */

#include "font.h"
#include "scene_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

/* ---- big-endian byte writer over a fixed buffer ------------------------ */
static uint8_t  g_ttf[4096];
static uint32_t g_len;
static void w8(uint8_t v)  { g_ttf[g_len++] = v; }
static void w16(uint16_t v){ w8((uint8_t)(v>>8)); w8((uint8_t)v); }
static void w32(uint32_t v){ w16((uint16_t)(v>>16)); w16((uint16_t)v); }
static void wi16(int16_t v){ w16((uint16_t)v); }
static void w16_at(uint32_t off, uint16_t v){ g_ttf[off]=(uint8_t)(v>>8); g_ttf[off+1]=(uint8_t)v; }
static void w32_at(uint32_t off, uint32_t v){ w16_at(off,(uint16_t)(v>>16)); w16_at(off+2,(uint16_t)v); }

static int alpha_px(struct render_target *rt, int x, int y) {
    if (x < 0 || y < 0 || x >= (int)rt->width || y >= (int)rt->height) return 0;
    uint32_t p = ((uint32_t *)((unsigned char *)rt->pixels + (size_t)y * rt->stride))[x];
    return (int)((p >> 24) & 255);
}

/* ---- build the synthetic font; returns total length ------------------- */
/* with_cmap12: a second, (3,10) format-12 subtable beside the format-4 one:
 * 'A'-'C' as before, U+1F600 -> 2, U+F0001..U+F0002 -> 1, and U+10FFF0 ->
 * glyph 99, which the font does not have. */
static uint32_t build_ttf_with(int with_cmap12) {
    g_len = 0;
    const int n_tables = 7;

    /* ---- offset table + (empty) directory we backpatch ---- */
    w32(0x00010000);            /* sfnt version */
    w16(n_tables);
    w16(0); w16(0); w16(0);     /* searchRange/entrySelector/rangeShift (unused by parser) */
    uint32_t dir = g_len;
    for (int i = 0; i < n_tables; i++) { w32(0); w32(0); w32(0); w32(0); }  /* 16 bytes each */

    struct { const char *tag; uint32_t off, len; } rec[7];
    int ri = 0;
    #define BEGIN(TAG) do { rec[ri].tag = (TAG); rec[ri].off = g_len; } while (0)
    #define END()      do { rec[ri].len = g_len - rec[ri].off; \
                            while (g_len & 3) { w8(0); } ri++; } while (0)

    /* head (54 bytes): unitsPerEm@18, indexToLocFormat@50 */
    BEGIN("head");
    { uint32_t s = g_len; for (int i=0;i<54;i++) w8(0);
      w32_at(s+0, 0x00010000);            /* version */
      w16_at(s+18, 1000);                 /* unitsPerEm */
      w16_at(s+50, 1); }                  /* indexToLocFormat = 1 (uint32 loca) */
    END();

    /* maxp (6 bytes): numGlyphs@4 = 4 */
    BEGIN("maxp");
    w32(0x00010000); w16(4);
    END();

    /* hhea (36 bytes): numberOfHMetrics@34 = 4 */
    BEGIN("hhea");
    { uint32_t s = g_len; for (int i=0;i<36;i++) w8(0); w16_at(s+34, 4); }
    END();

    /* hmtx: 4 longHorMetric {advanceWidth, lsb} */
    BEGIN("hmtx");
    w16(500); w16(100);   /* glyph 0 .notdef */
    w16(500); w16(100);   /* glyph 1 square  */
    w16(500); w16(100);   /* glyph 2 curve   */
    w16(550); w16(100);   /* glyph 3 composite */
    END();

    /* cmap: header + one (3,1) format-4 subtable. 'A'->1 'B'->2 'C'->3 */
    BEGIN("cmap");
    { uint32_t cmap_start = g_len;
      w16(0); w16(with_cmap12 ? 2 : 1);     /* version, numTables */
      w16(3); w16(1); uint32_t off4 = g_len; w32(0);     /* platform 3, enc 1 */
      uint32_t off12 = 0;
      if (with_cmap12) { w16(3); w16(10); off12 = g_len; w32(0); }
      w32_at(off4, g_len - cmap_start);
      /* format 4, segCount = 2 (real seg + 0xFFFF terminator) */
      w16(4);                               /* format */
      uint32_t lenoff = g_len; w16(0);      /* length (backpatch) */
      w16(0);                               /* language */
      w16(4);                               /* segCountX2 */
      w16(4); w16(1); w16(0);               /* searchRange/entrySelector/rangeShift */
      w16(0x0043); w16(0xFFFF);             /* endCode[]   */
      w16(0);                               /* reservedPad */
      w16(0x0041); w16(0xFFFF);             /* startCode[] */
      wi16(-64);  w16(1);                   /* idDelta[] : 0x41-64=1, 0x42..=2, 0x43..=3 */
      w16(0); w16(0);                       /* idRangeOffset[] (0 => direct) */
      w16_at(lenoff, (uint16_t)(g_len - lenoff + 2));      /* subtable length */
      if (with_cmap12) {
          w32_at(off12, g_len - cmap_start);
          static const uint32_t groups[][3] = {{0x41,0x43,1},{0x1F600,0x1F600,2},{0xF0001,0xF0002,1},{0x10FFF0,0x10FFF0,99}};
          w16(12); w16(0); w32(16 + 12 * 4); w32(0); w32(4);
          for (int i = 0; i < 4; i++) { w32(groups[i][0]); w32(groups[i][1]); w32(groups[i][2]); }
      }
    }
    END();

    /* glyf: glyph0 empty, glyph1 square, glyph2 curve, glyph3 composite */
    uint32_t g_off[5];
    BEGIN("glyf");
    { uint32_t glyf_start = g_len;
      g_off[0] = g_len - glyf_start;        /* glyph 0: empty */
      g_off[1] = g_len - glyf_start;

      /* glyph 1: square (100,100)-(400,400), 4 on-curve points */
      wi16(1);                              /* numberOfContours */
      wi16(100); wi16(100); wi16(400); wi16(400);   /* bbox */
      w16(3);                               /* endPtsOfContours[0] */
      w16(0);                               /* instructionLength */
      w8(0x01); w8(0x01); w8(0x01); w8(0x01);        /* flags: all ON_CURVE, int16 deltas */
      wi16(100); wi16(300); wi16(0); wi16(-300);     /* x deltas */
      wi16(100); wi16(0); wi16(300); wi16(0);        /* y deltas */
      g_off[2] = g_len - glyf_start;

      /* glyph 2: quadratic curve, on(100,100) off(250,400) on(400,100) */
      wi16(1);
      wi16(100); wi16(100); wi16(400); wi16(400);
      w16(2);                               /* endPts: 3 points (0..2) */
      w16(0);
      w8(0x01); w8(0x00); w8(0x01);         /* on, OFF-curve, on */
      wi16(100); wi16(150); wi16(150);      /* x: 100,250,400 */
      wi16(100); wi16(300); wi16(-300);     /* y: 100,400,100 */
      g_off[3] = g_len - glyf_start;

      /* glyph 3: composite -> glyph 1 offset by (50,25) */
      wi16(-1);
      wi16(150); wi16(125); wi16(450); wi16(425);
      w16(0x0003);                          /* ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES */
      w16(1);                               /* component glyphIndex */
      wi16(50); wi16(25);                   /* dx, dy */
      g_off[4] = g_len - glyf_start;
    }
    END();

    /* loca: 5 uint32 offsets (index_to_loc_fmt = 1) */
    BEGIN("loca");
    for (int i = 0; i < 5; i++) w32(g_off[i]);
    END();

    /* backpatch the directory */
    for (int i = 0; i < n_tables; i++) {
        uint32_t r = dir + (uint32_t)i * 16;
        g_ttf[r]   = (uint8_t)rec[i].tag[0]; g_ttf[r+1] = (uint8_t)rec[i].tag[1];
        g_ttf[r+2] = (uint8_t)rec[i].tag[2]; g_ttf[r+3] = (uint8_t)rec[i].tag[3];
        w32_at(r + 4, 0);                   /* checksum (unvalidated) */
        w32_at(r + 8, rec[i].off);
        w32_at(r + 12, rec[i].len);
    }
    return g_len;
}
static uint32_t build_ttf(void) { return build_ttf_with(0); }

/* ---- T1: parse + rasterize a known square ------------------------------ */
static uint32_t t1_load_and_square(void) {
    printf("T1 parse + rasterize a synthetic square:\n");
    uint32_t len = build_ttf();
    uint32_t fh = font_load(g_ttf, len);
    CHECK(fh != 0, "font_load succeeds on the synthetic ttf");
    struct font *f = font_for_handle(fh);
    CHECK(f != 0 && f->units_per_em == 1000, "head parsed (units_per_em=1000)");
    CHECK(f && f->num_glyphs == 4, "maxp parsed (num_glyphs=4)");

    CHECK(font_codepoint_to_glyph(f, 'A') == 1, "cmap: 'A' -> glyph 1");
    CHECK(font_codepoint_to_glyph(f, 'B') == 2, "cmap: 'B' -> glyph 2");
    CHECK(font_codepoint_to_glyph(f, 'Z') == 0, "cmap: unmapped 'Z' -> .notdef 0");

    struct glyph_outline o;
    CHECK(font_get_glyph_outline(f, 1, &o), "get square outline");
    CHECK(o.n_contours == 1 && o.contours[0].n_points == 4, "square: 1 contour, 4 points");
    int all_on = 1;
    for (uint32_t i = 0; i < o.contours[0].n_points; i++) if (!o.contours[0].points[i].on_curve) all_on = 0;
    CHECK(all_on, "square: all points on-curve");
    font_free_outline(&o);

    uint8_t *cov = 0; int w=0,h=0,bx=0,by=0; float adv=0;
    CHECK(font_rasterize_glyph(f, 1, 100.0f, &cov, &w, &h, &bx, &by, &adv), "rasterize square @100px");
    /* 300 font units * (100/1000) = 30px square */
    CHECK(w >= 28 && w <= 32 && h >= 28 && h <= 32, "square bitmap ~30x30");
    CHECK(cov && cov[(h/2)*w + w/2] >= 250, "square center fully covered");
    /* a corner-ish sample inside, and outside-of-bitmap is by definition uncovered */
    CHECK(cov && cov[2*w + 2] >= 250, "square interior covered");
    free(cov);
    /* EXACT AREA: at 103 px the square spans x 10.3..41.2, so its left column
     * is 70% covered and its right one 20% -- 179 and 51. Sampling 4 x 4 gave
     * 191 and 64: only the multiples of a quarter it could see. */
    cov = 0;
    CHECK(font_rasterize_glyph(f, 1, 103.0f, &cov, &w, &h, &bx, &by, &adv), "rasterize square @103px");
    if (cov) {
        int l = cov[(h/2)*w], r = cov[(h/2)*w + w - 1];
        printf("        (edge columns: %d and %d)\n", l, r);
        CHECK(l >= 178 && l <= 180, "a 70%-covered edge pixel is 179, the area it covers");
        CHECK(r >= 50 && r <= 52, "a 20%-covered one is 51");
    }
    free(cov);
    /* SUBPIXEL: at 100 px the square's left edge is on a whole pixel; moved
     * half a pixel right it half-covers that column instead */
    cov = 0;
    CHECK(font_rasterize_glyph_at(f, 1, 100.0f, 0.5f, &cov, &w, &h, &bx, &by, &adv), "rasterize square half a pixel right");
    if (cov) {
        int l = cov[(h/2)*w];
        CHECK(bx == 10 && l >= 126 && l <= 129, "half a pixel right: the edge column is half covered (128)");
        CHECK(adv == 50.0f, "and the advance is the advance at every phase");
    }
    free(cov);
    struct glyph_cache_entry *p0 = glyph_cache_lookup_phase(font_global_atlas(), f, 'A', 40.0f, 0);
    uint16_t p0x = p0 ? p0->atlas_x : 0, p0y = p0 ? p0->atlas_y : 0;
    struct glyph_cache_entry *p2 = glyph_cache_lookup_phase(font_global_atlas(), f, 'A', 40.0f, 2);
    CHECK(p0 && p2 && p2->phase == 2 && (p2->atlas_x != p0x || p2->atlas_y != p0y),
          "each phase is its own cache entry, with its own pixels");
    return fh;
}

/* ---- T2: quadratic flattening actually curves -------------------------- */
static void t2_curve(struct font *f) {
    printf("T2 quadratic curve flattening:\n");
    struct glyph_outline o;
    CHECK(font_get_glyph_outline(f, 2, &o), "get curve outline");
    int has_off = 0;
    for (uint32_t i = 0; i < o.contours[0].n_points; i++) if (!o.contours[0].points[i].on_curve) has_off = 1;
    CHECK(has_off, "curve glyph has an off-curve control point");
    font_free_outline(&o);

    /* flatten contour 0 and confirm the polyline bulges off the endpoint chord.
     * Endpoints share y=100 (font units); the control at y=400 pulls the curve
     * to y~250 at its midpoint -- a straight-line replay would keep y flat. */
    float xs[512], ys[512];
    int n = font_debug_flatten_contour0(f, 2, 100.0f / 1000.0f, xs, ys, 512);
    CHECK(n > 4, "flattening produced intermediate polyline points");
    float ymin = 1e30f, ymax = -1e30f;
    for (int i = 0; i < n; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    /* y spread in pixels should be ~ (250-100)*0.1 = 15px if the curve bulges;
     * a straight chord (all endpoints y=100) would spread ~0. */
    CHECK((ymax - ymin) > 8.0f, "flattened curve deviates from the straight chord");
}

/* ---- T3: composite glyph offset ---------------------------------------- */
static void t3_composite(struct font *f) {
    printf("T3 composite glyph offset:\n");
    struct glyph_outline sq, comp;
    CHECK(font_get_glyph_outline(f, 1, &sq), "get base square");
    CHECK(font_get_glyph_outline(f, 3, &comp), "get composite");
    CHECK(comp.n_contours == 1 && comp.contours[0].n_points == 4, "composite: 1 contour, 4 points (from base)");
    int shifted_ok = 1;
    for (uint32_t i = 0; i < 4 && i < comp.contours[0].n_points; i++) {
        float ex = sq.contours[0].points[i].x + 50.0f;
        float ey = sq.contours[0].points[i].y + 25.0f;
        if (comp.contours[0].points[i].x != ex || comp.contours[0].points[i].y != ey) shifted_ok = 0;
    }
    CHECK(shifted_ok, "composite points == base square shifted by exactly (50,25)");
    font_free_outline(&sq); font_free_outline(&comp);
}

/* ---- T4: atlas caching (no re-rasterize on repeat lookup) -------------- */
static void t4_atlas_cache(struct font *f) {
    printf("T4 atlas cache is a real cache:\n");
    struct glyph_atlas *atlas = font_global_atlas();
    uint32_t before = font_debug_rasterize_count();
    struct glyph_cache_entry *e1 = glyph_cache_lookup_or_rasterize(atlas, f, 'A', 32.0f);
    uint32_t mid = font_debug_rasterize_count();
    struct glyph_cache_entry *e2 = glyph_cache_lookup_or_rasterize(atlas, f, 'A', 32.0f);
    uint32_t after = font_debug_rasterize_count();

    CHECK(e1 && e2, "both lookups returned an entry");
    CHECK(mid == before + 1, "first lookup rasterized once");
    CHECK(after == mid, "second lookup did NOT rasterize (cache hit)");
    CHECK(e1->atlas_x == e2->atlas_x && e1->atlas_y == e2->atlas_y, "same atlas slot on hit");
}

/* ---- T5: pen advance + multi-glyph layout ------------------------------ */
static void t5_pen_advance(struct font *f, uint32_t fh) {
    printf("T5 pen advance + multi-glyph layout:\n");
    font_install_backend();
    struct render_backend *be = cpu_backend_get();
    CHECK(be->draw_text != 0, "draw_text installed into the vtable");

    /* render "AAA" and confirm three inked squares spaced by the advance. */
    int W = 128, H = 64;
    /* ZEROED FIRST. render_target has fields beyond the five set here --
     * clear_dirty among them -- and leaving them as stack garbage makes the
     * test depend on what happened to be on the stack. That is not
     * theoretical: a non-zero clear_dirty makes begin_frame() wipe the target,
     * so the checkerboard T2 paints was being erased before the blur sampled
     * it, and the blur "failed" by reading the zeros it had been handed. */
    struct render_target rt;
    memset(&rt, 0, sizeof rt);
    rt.pixels = calloc((size_t)W*H, 4); rt.width = W; rt.height = H; rt.stride = W*4;
    rt.format = EMBK_PIXFMT_BGRA8888_PRE;
    be->begin_frame(&rt, NULL, 0);
    struct color white = {1,1,1,1};
    /* draw_text grew a paint and a box after this test was written: a
     * gradient-stroked glyph needs the box it is stroked over. NULL paint is
     * the plain-fill case this test has always been about. */
    be->draw_text(&rt, 4, 40, "AAA", fh, 50.0f, white, 1.0f, NULL, 0.0f, 0.0f,false);
    be->end_frame(&rt);

    /* advance = 500 * 50/1000 = 25px; square 'A' ~ x in [bearing, bearing+15] */
    struct glyph_cache_entry *g = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, 'A', 50.0f);
    int adv = (int)(g->advance_px + 0.5f);
    CHECK(adv >= 24 && adv <= 26, "advance ~25px");

    int cy = 25;   /* somewhere inside the glyph rows */
    int a0 = alpha_px(&rt, 4 + g->bearing_x + 5, cy);
    int a1 = alpha_px(&rt, 4 + adv + g->bearing_x + 5, cy);
    int a2 = alpha_px(&rt, 4 + 2*adv + g->bearing_x + 5, cy);
    CHECK(a0 > 40, "glyph 0 inked at pen origin");
    CHECK(a1 > 40, "glyph 1 inked one advance over");
    CHECK(a2 > 40, "glyph 2 inked two advances over");

    free(rt.pixels);
}

static void t6_wrapped_pixels(struct font *f,uint32_t fh) {
    int16_t ascent=f->ascent,descent=f->descent;
    f->ascent=800;f->descent=200;
    struct render_target rt={0};rt.width=128;rt.height=80;rt.stride=128*4;
    rt.pixels=calloc(128*80,4);rt.format=EMBK_PIXFMT_BGRA8888_PRE;
    struct render_backend *be=cpu_backend_get();be->begin_frame(&rt,NULL,0);
    be->draw_text(&rt,0,0,"A A",fh,20,(struct color){1,1,1,1},1,NULL,11,60,true);
    be->end_frame(&rt);
    int ink=0;for(int y=20;y<60;y++)for(int x=0;x<128;x++)ink+=alpha_px(&rt,x,y)>0;
    CHECK(ink>0,"wrapped text paints glyphs on the second measured line");
    memset(rt.pixels,0,128*80*4);be->begin_frame(&rt,NULL,0);
    be->draw_text(&rt,0,0,"A A",fh,20,(struct color){1,1,1,1},1,NULL,11,60,false);
    be->end_frame(&rt);ink=0;
    for(int y=20;y<60;y++)for(int x=0;x<128;x++)ink+=alpha_px(&rt,x,y)>0;
    CHECK(ink==0,"preformatted input and code remain on one line");
    CHECK(font_layout_text(f,"A  A",20,40,NULL,NULL)==1 && font_layout_text(f,"A  A",20,39,NULL,NULL)==2,
          "measurement preserves repeated spaces at the wrapping boundary");
    CHECK(font_layout_text(f,"A\nA",20,0,NULL,NULL)==2,"explicit line breaks share the same line walk");
    free(rt.pixels);f->ascent=ascent;f->descent=descent;
}

/* ---- T7: cmap format 12, chosen over format 4, every plane ------------- */
static uint8_t g_ttf12[4096];
static void t7_cmap12(void) {
    printf("T7 cmap format 12:\n");
    uint32_t len = build_ttf_with(1);
    memcpy(g_ttf12, g_ttf, len);
    uint32_t fh = font_load(g_ttf12, len);
    struct font *f = font_for_handle(fh);
    CHECK(f && f->cmap_format == 12, "the format-12 subtable is chosen over format 4");
    if (!f) return;
    CHECK(font_codepoint_to_glyph(f, 'A') == 1 && font_codepoint_to_glyph(f, 'C') == 3, "BMP codepoints through format 12");
    CHECK(font_codepoint_to_glyph(f, 0x1F600) == 2, "a supplementary-plane codepoint (U+1F600)");
    CHECK(font_codepoint_to_glyph(f, 0xF0002) == 2 && font_codepoint_to_glyph(f, 0xF0003) == 0,
          "a range in plane 15 maps by offset and ends where it ends");
    CHECK(font_codepoint_to_glyph(f, 0x10FFF0) == 0, "a glyph past numGlyphs is no glyph");

    /* THE FALLBACK: a face without U+1F600 borrows it from one with it */
    static uint8_t g_ttf4[4096];
    uint32_t len4 = build_ttf_with(0);
    memcpy(g_ttf4, g_ttf, len4);
    uint32_t ph = font_load(g_ttf4, len4);
    struct font *p = font_for_handle(ph);
    CHECK(p && font_for_codepoint(p, 0x1F600) == p, "with no fallback, a missing codepoint stays with its face");
    font_set_fallback(ph, fh);
    CHECK(p && font_for_codepoint(p, 0x1F600) == f, "with one, the fallback draws what the face lacks");
    CHECK(p && font_for_codepoint(p, 'A') == p, "and the face still draws what it has");
    CHECK(p && font_for_codepoint(p, 0xF0003) == p, "a codepoint neither has stays with the face (its .notdef)");
    font_set_fallback(ph, ph);
    CHECK(p && p->fallback == fh, "a face is never its own fallback");
    CHECK(font_has_glyph(f, 0x1F600) && !font_has_glyph(f, 0x40), "font_has_glyph");
}

/* ---- T8: the cache under pressure: pages fill, the oldest is emptied, ---- *
 * ---- what is in use stays, and nothing live is overwritten.           ---- */
static uint32_t coverage_sum(const struct glyph_cache_entry *e) {
    const uint8_t *c = glyph_coverage(font_global_atlas(), e);
    uint32_t sum = 0;
    for (int y = 0; c && y < e->atlas_h; y++)
        for (int x = 0; x < e->atlas_w; x++) sum = sum * 31 + c[(size_t)y * GLYPH_ATLAS_SIZE + x];
    return sum;
}
static void t8_cache_pressure(struct font *f) {
    printf("T8 the glyph cache under pressure:\n");
    struct glyph_atlas *atlas = font_global_atlas();
    struct glyph_cache_entry *hot = glyph_cache_lookup_or_rasterize(atlas, f, 'B', 21.0f);
    uint32_t hot_sum = coverage_sum(hot), hot_misses = 0, evicted_seen = 0;
    CHECK(hot && hot_sum, "a glyph with pixels");
    struct glyph_cache_entry *missing = glyph_cache_lookup_or_rasterize(atlas, f, 0x1234, 21.0f);
    CHECK(missing && missing->missing, "a codepoint the font lacks is flagged missing");
    CHECK(!glyph_cache_lookup_or_rasterize(atlas, f, 'A', 21.0f)->missing, "one it has is not");
    for (int size = 8; size <= 700; size++) {
        for (uint32_t cp = 'A'; cp <= 'C'; cp++) glyph_cache_lookup_or_rasterize(atlas, f, cp, (float)size);
        uint32_t before = font_debug_rasterize_count();
        hot = glyph_cache_lookup_or_rasterize(atlas, f, 'B', 21.0f);   /* used every round */
        hot_misses += font_debug_rasterize_count() != before;
    }
    int pages = 0;
    for (int p = 0; p < GLYPH_ATLAS_PAGES; p++) pages += atlas->pages[p].coverage != 0;
    CHECK(pages == GLYPH_ATLAS_PAGES, "the churn filled every page");
    CHECK(hot_misses == 0, "the glyph used every frame was never evicted");
    CHECK(coverage_sum(hot) == hot_sum, "and its pixels were never overwritten");
    /* every live entry is found again without rasterizing; the evicted come back */
    int consistent = 1;
    uint32_t gone_cp = 0; float gone_size = 0;
    for (uint32_t i = 0; i < atlas->n_entries; i++) {
        struct glyph_cache_entry *e = &atlas->entries[i];
        if (!e->used) { if (!evicted_seen++) { gone_cp = e->codepoint; gone_size = e->size_px_x4 / 4.0f; } continue; }
        uint32_t before = font_debug_rasterize_count();
        struct glyph_cache_entry *again = glyph_cache_lookup_phase(atlas, f, e->codepoint, e->size_px_x4 / 4.0f, e->phase);
        if (again != e || font_debug_rasterize_count() != before) consistent = 0;
    }
    CHECK(consistent, "every cached glyph is found by its key");
    CHECK(evicted_seen > 0, "pages were emptied to make room");
    /* an evicted glyph (its entry keeps its key until reused) comes back */
    uint8_t *cov = 0; int w = 0, h = 0, bx = 0, by = 0; float adv = 0;
    font_rasterize_glyph(f, font_codepoint_to_glyph(f, gone_cp), gone_size, &cov, &w, &h, &bx, &by, &adv);
    free(cov);
    uint32_t before = font_debug_rasterize_count();
    struct glyph_cache_entry *back = glyph_cache_lookup_or_rasterize(atlas, f, gone_cp, gone_size);
    CHECK(font_debug_rasterize_count() == before + 1 && back && back->atlas_w == w && back->atlas_h == h,
          "an evicted glyph is rasterized again, at its own size");
}

/* ---- T9: the vendored terminal face, from the tree ---------------------- */
static void t9_meslo(void) {
    printf("T9 MesloLGS Nerd Font Mono (third_party/fonts/meslo-nf):\n");
    FILE *file = fopen("third_party/fonts/meslo-nf/MesloLGSNerdFontMono-Regular.ttf", "rb");
    CHECK(file != 0, "the vendored face is in the tree");
    if (!file) return;
    fseek(file, 0, SEEK_END); long n = ftell(file); fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, file) == (size_t)n, "read it");
    fclose(file);
    struct font *f = font_for_handle(font_load(data, (size_t)n));
    CHECK(f && f->cmap_format == 12, "it loads, with its format-12 cmap");
    if (!f) return;
    CHECK(font_has_glyph(f, 'A') && font_has_glyph(f, 0xE0B0) && font_has_glyph(f, 0xE0B6),
          "letters and the powerline separators");
    CHECK(font_has_glyph(f, 0xF0001) && font_has_glyph(f, 0xF0A1A), "Material Design icons in plane 15");
    CHECK(font_has_glyph(f, 0xF113) && font_has_glyph(f, 0xE718), "the icons p10k's segments draw (git, os)");
    CHECK(!font_has_glyph(f, 0x754C), "no CJK: the fallback chain's job");
    struct glyph_atlas *atlas = font_global_atlas();
    float m = glyph_cache_lookup_or_rasterize(atlas, f, 'M', 16)->advance_px;
    float pl = glyph_cache_lookup_or_rasterize(atlas, f, 0xE0B0, 16)->advance_px;
    float md = glyph_cache_lookup_or_rasterize(atlas, f, 0xF0001, 16)->advance_px;
    CHECK(m > 8 && m == pl && m == md, "every glyph is one cell wide (the Mono build)");
    struct glyph_cache_entry *sep = glyph_cache_lookup_or_rasterize(atlas, f, 0xE0B0, 16);
    CHECK(sep->atlas_w > 4 && sep->atlas_h > 12 && !sep->missing, "the separator rasterizes");
}

/* ---- T10: the interface's face, and its kerning ------------------------- */
static float g_kx[8];
static int g_kn;
static void kern_visit(void *ctx, uint32_t cp, float x, float y) { (void)ctx; (void)cp; (void)y; if (g_kn < 8) g_kx[g_kn++] = x; }
static void t10_inter(void) {
    printf("T10 Inter and its kerning (third_party/fonts/inter):\n");
    FILE *file = fopen("third_party/fonts/inter/Inter-Regular.ttf", "rb");
    CHECK(file != 0, "the vendored face is in the tree");
    if (!file) return;
    fseek(file, 0, SEEK_END); long n = ftell(file); fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, file) == (size_t)n, "read it");
    fclose(file);
    struct font *f = font_for_handle(font_load(data, (size_t)n));
    CHECK(f && f->gpos_len > 0 && f->n_kern_lookups > 0, "it loads, with GPOS and a 'kern' feature");
    if (!f) return;
    float av = font_kern_px(f, 'A', 'V', 100), to = font_kern_px(f, 'T', 'o', 100);
    float vertical = font_kern_px(f, 'H', 'H', 100);
    printf("        (at 100 px: AV %.2f, To %.2f, HH %.2f)\n", av, to, vertical);
    CHECK(av < -2.0f && to < -2.0f, "AV and To sit closer than their advances say");
    CHECK(vertical == 0.0f, "two upright stems keep their spacing (HH is not kerned)");
    CHECK(font_kern_px(f, 0x00C5, 'V', 100) < -2.0f, "an accented capital (\u00c5, outside the ASCII table) kerns as A does");
    CHECK(font_kern_px(f, 'A', 'V', 20) * 5 > av - 0.3f && font_kern_px(f, 'A', 'V', 20) * 5 < av + 0.3f,
          "kerning scales with the size");
    CHECK(font_kern_px(f, 0, 'V', 100) == 0.0f, "nothing before the first glyph");
    /* the text walk places the V at A's advance plus the pair's kerning */
    g_kn = 0;
    font_layout_text(f, "AV", 100, 0, kern_visit, 0);
    float adv_a = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, 'A', 100)->advance_px;
    float pair = font_pair_px(f, 'A', 'V', 100);   /* the kerning, and Inter's tracking (T11) */
    CHECK(g_kn == 2 && g_kx[1] > adv_a + pair - 0.01f && g_kx[1] < adv_a + pair + 0.01f,
          "the text walk puts V at A's advance plus the kerning");
    /* a box exactly as wide as the kerned word holds all of it: the fit test
     * once left the pair's kerning out and wrapped the last letter away
     * ("Keyboar", "Slat" in Settings) */
    const char *words[] = { "Keyboard", "Slate", "preview", "Update", "AVATAR" };
    int whole = 1;
    for (int i = 0; i < 5; i++) {
        float w = 0; uint32_t prev = 0;
        for (const char *p = words[i]; *p; p++) {
            w += font_pair_px(f, prev, (uint32_t)*p, 14) + glyph_cache_lookup_or_rasterize(font_global_atlas(), f, (uint32_t)*p, 14)->advance_px;
            prev = (uint32_t)*p;
        }
        if (font_layout_text(f, words[i], 14, w, NULL, NULL) != 1) { whole = 0; printf("        (\"%s\" wrapped at its own width %.3f)\n", words[i], w); }
        g_kn = 0;
        if (font_layout_text(f, words[i], 14, w, kern_visit, 0) != 1) whole = 0;
    }
    CHECK(whole, "a word in a box its own kerned width stays on one line, measured and drawn");

    /* T11: Inter's dynamic tracking -- large type closes up, small type keeps
     * its air -- on every pair the walks add (font_pair_px), and nowhere else */
    printf("T11 Inter's dynamic tracking:\n");
    CHECK(f->dyn_tracking, "the face is recognised as Inter by its family name");
    float t32 = font_pair_px(f, 'H', 'H', 32), t11 = font_pair_px(f, 'H', 'H', 11), t13 = font_pair_px(f, 'H', 'H', 13);
    printf("        (HH at 32 px %.3f, 13 px %.3f, 11 px %.3f)\n", t32, t13, t11);
    CHECK(t32 < -0.6f && t32 > -0.8f, "at 32 px a title closes up by about 0.7 px a pair");
    CHECK(t11 > 0.0f && t11 < 0.1f, "at 11 px a caption opens a touch");
    CHECK(t13 > -0.1f && t13 < 0.0f, "at 13 px body text is all but untouched");
    CHECK(font_pair_px(f, 'A', 'V', 32) < font_kern_px(f, 'A', 'V', 32), "kerning and tracking add up");
    CHECK(font_pair_px(f, 0, 'V', 32) == 0.0f, "nothing before the first glyph");
    g_kn = 0;
    font_layout_text(f, "HH", 32, 0, kern_visit, 0);
    float adv_h = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, 'H', 32)->advance_px;
    CHECK(g_kn == 2 && g_kx[1] > adv_h + t32 - 0.01f && g_kx[1] < adv_h + t32 + 0.01f,
          "the text walk spaces the letters by it");
}

int main(void) {
    printf("=== EmbLink UI Piece 4b: font-rasterizer selftests ===\n");
    uint32_t fh = t1_load_and_square();
    struct font *f = font_for_handle(fh);
    if (f) {
        t2_curve(f);
        t3_composite(f);
        t4_atlas_cache(f);
        t5_pen_advance(f, fh);
        t6_wrapped_pixels(f,fh);
        t8_cache_pressure(f);
    } else { printf("  FAIL: no font handle; skipping T2-T5\n"); g_fail++; }
    t7_cmap12();
    t9_meslo();
    t10_inter();
    printf("=== font-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
