/* virgl_compose.c -- the desktop composed on the host's GPU. See virgl_compose.h. */
#include "drivers/video/virgl_compose.h"
#include "drivers/video/virgl.h"
#include "include/kstring.h"

#define SUB_CTX 1u
/* Object handles: one namespace per sub-context, whatever the type. */
enum {
    H_BLEND_OVER = 1, H_BLEND_COPY, H_DSA, H_RAST, H_VE, H_SAMP_NEAREST, H_SAMP_LINEAR,
    H_VS = 10, H_FS_IMAGE, H_FS_SHADOW, H_FS_SOLID, H_FS_BLUR_RECT, H_FS_BLUR_2D,
    H_TARGET = 20, H_TARGET_VIEW, H_BLUR_SURF, H_BLUR_VIEW,
    H_IMAGE = 1000,             /* image i: 1000 + 2i as it is, 1001 + 2i opaque */
};
#define MAX_IMAGES   96u
#define VB_BYTES     (256u * 1024u)
#define VERTEX_BYTES 48u        /* three vec4 attributes */
#define QUAD_BYTES   (4u * VERTEX_BYTES)
#define Y_0_TOP      1u         /* resource flag: transfers turn the rows over */

static const struct vgc_transport *T;
static uint32_t stream_buf[VG_STREAM_MAX_DWORDS];
static struct vg_stream S = { stream_buf, 0, VG_STREAM_MAX_DWORDS, 0 };
static int ready, failed, probing;
static uint32_t vb_res, vb_off;
static uint32_t target_res, target_w, target_h;      /* what H_TARGET is a surface of */
static int target_y0_top;
static struct gpu_rect damage;
static uint32_t blur_res, blur_w, blur_h;
static struct { uint8_t used; uint32_t res, w, h, stride; } img[MAX_IMAGES];

/* ---- shaders ------------------------------------------------------------------ */
/* Pixel coordinates in IN[0] become clip coordinates by CONST[0] =
 * (2/W, -2/H, -1, 1); the texture coordinate and the frame-local position
 * pass through. */
static const char vs_text[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL IN[2]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "DCL OUT[2], GENERIC[1]\n"
    "DCL CONST[0]\n"
    "DCL TEMP[0]\n"
    "IMM[0] FLT32 { 0.0000, 1.0000, 0.0000, 0.0000}\n"
    "  0: MAD TEMP[0].xy, IN[0].xyyy, CONST[0].xyyy, CONST[0].zwww\n"
    "  1: MOV TEMP[0].zw, IMM[0].xxxy\n"
    "  2: MOV OUT[0], TEMP[0]\n"
    "  3: MOV OUT[1], IN[1]\n"
    "  4: MOV OUT[2], IN[2]\n"
    "  5: END\n";

/* The distance from a pixel to a rounded rectangle, d < 0 inside, from a
 * position `in` relative to the rectangle's corner, `half` = half its size
 * and `rad` = the corner radius: q = |p - half| - half + r;
 * d = |max(q, 0)| + min(max(q.x, q.y), 0) - r. Leaves d in TEMP[2].x and
 * uses TEMP[1..3]; the shader's IMM[0].x must be 0. */
#define SDF(in, half, rad)                                             \
    "  ADD TEMP[1].xy, " in ".xyyy, -" half ".xyyy\n"                  \
    "  ADD TEMP[1].xy, |TEMP[1].xyyy|, -" half ".xyyy\n"               \
    "  ADD TEMP[1].xy, TEMP[1].xyyy, " rad "\n"                        \
    "  MAX TEMP[2].xy, TEMP[1].xyyy, IMM[0].xxxx\n"                    \
    "  DP2 TEMP[2].x, TEMP[2].xyyy, TEMP[2].xyyy\n"                    \
    "  SQRT TEMP[2].x, TEMP[2].xxxx\n"                                 \
    "  MAX TEMP[3].x, TEMP[1].xxxx, TEMP[1].yyyy\n"                    \
    "  MIN TEMP[3].x, TEMP[3].xxxx, IMM[0].xxxx\n"                     \
    "  ADD TEMP[2].x, TEMP[2].xxxx, TEMP[3].xxxx\n"                    \
    "  ADD TEMP[2].x, TEMP[2].xxxx, -" rad "\n"

/* An image: texel, border ring, then coverage x opacity.
 * CONST[0] = (opacity, radius, ring width + 0.5 or -1000, -), CONST[1] = half
 * frame, CONST[2] = ring colour, premultiplied. */
static const char fs_image_text[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], LINEAR\n"
    "DCL IN[1], GENERIC[1], LINEAR\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], RECT, FLOAT\n"
    "DCL CONST[0..2]\n"
    "DCL TEMP[0..3]\n"
    "IMM[0] FLT32 { 0.0000, 0.5000, 1.0000, 0.0000}\n"
    "  TEX TEMP[0], IN[0], SAMP[0], RECT\n"
    SDF("IN[1]", "CONST[1]", "CONST[0].yyyy")
    "  ADD_SAT TEMP[3].x, IMM[0].yyyy, -TEMP[2].xxxx\n"     /* coverage = sat(0.5 - d) */
    "  ADD_SAT TEMP[3].y, TEMP[2].xxxx, CONST[0].zzzz\n"    /* ring = sat(d + width + 0.5) */
    "  MUL TEMP[3].z, TEMP[3].yyyy, CONST[2].wwww\n"
    "  ADD TEMP[3].w, IMM[0].zzzz, -TEMP[3].zzzz\n"
    "  MUL TEMP[0], TEMP[0], TEMP[3].wwww\n"
    "  MAD TEMP[0], CONST[2], TEMP[3].yyyy, TEMP[0]\n"
    "  MUL TEMP[3].x, TEMP[3].xxxx, CONST[0].xxxx\n"
    "  MUL OUT[0], TEMP[0], TEMP[3].xxxx\n"
    "  END\n";

/* A shadow: black, peak * (1 - e / width)^2 where e is how far a pixel's far
 * edge is from the casting frame (0 inside it), and none where the cover --
 * the window it lies under -- is: a glass window must not show its own
 * shadow through itself. CONST[0] = (peak, radius, 1 / width, -), CONST[1] =
 * half frame, CONST[2].xy = the cover's corner from the frame's, CONST[3] =
 * half cover. */
static const char fs_shadow_text[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[1], LINEAR\n"
    "DCL OUT[0], COLOR\n"
    "DCL CONST[0..3]\n"
    "DCL TEMP[0..4]\n"
    "IMM[0] FLT32 { 0.0000, 0.5000, 1.0000, -0.5000}\n"
    SDF("IN[0]", "CONST[1]", "CONST[0].yyyy")
    "  MAX TEMP[4].x, TEMP[2].xxxx, IMM[0].wwww\n"
    "  ADD TEMP[4].x, TEMP[4].xxxx, IMM[0].yyyy\n"
    "  MUL TEMP[4].x, TEMP[4].xxxx, CONST[0].zzzz\n"
    "  ADD_SAT TEMP[4].x, IMM[0].zzzz, -TEMP[4].xxxx\n"
    "  MUL TEMP[4].x, TEMP[4].xxxx, TEMP[4].xxxx\n"
    "  ADD TEMP[0].xy, IN[0].xyyy, -CONST[2].xyyy\n"
    SDF("TEMP[0]", "CONST[3]", "CONST[0].yyyy")
    "  ADD_SAT TEMP[2].x, IMM[0].yyyy, -TEMP[2].xxxx\n"     /* the cover's coverage */
    "  ADD TEMP[2].x, IMM[0].zzzz, -TEMP[2].xxxx\n"
    "  MUL TEMP[4].x, TEMP[4].xxxx, TEMP[2].xxxx\n"
    "  MUL TEMP[0].w, TEMP[4].xxxx, CONST[0].xxxx\n"
    "  MOV TEMP[0].xyz, IMM[0].xxxx\n"
    "  MOV OUT[0], TEMP[0]\n"
    "  END\n";

static const char fs_solid_text[] =
    "FRAG\n"
    "DCL OUT[0], COLOR\n"
    "DCL CONST[0]\n"
    "  MOV OUT[0], CONST[0]\n"
    "  END\n";

/* One pass of a box blur: the mean of 2r + 1 texels along CONST[0].xy,
 * each clamped into CONST[1] = (min x, min y, max x, max y), the region's
 * own texel centres, so its edge repeats as the CPU blur's does; CONST[2].xy
 * then scales the coordinate for the sampler (1 for a rectangle texture, one
 * over the size for a 2D one). CONST[0] = (step x, step y, r, 1 / (2r + 1)).
 * The result is premultiplied by the coverage of a rounded frame, CONST[3] =
 * half of it and CONST[4].x its radius, from the frame-local IN[1]. */
#define FS_BLUR(target)                                                \
    "FRAG\n"                                                           \
    "DCL IN[0], GENERIC[0], LINEAR\n"                                  \
    "DCL IN[1], GENERIC[1], LINEAR\n"                                  \
    "DCL OUT[0], COLOR\n"                                              \
    "DCL SAMP[0]\n"                                                    \
    "DCL SVIEW[0], " target ", FLOAT\n"                                \
    "DCL CONST[0..4]\n"                                                \
    "DCL TEMP[0..3]\n"                                                 \
    "IMM[0] FLT32 { 0.0000, 1.0000, 0.5000, 0.0000}\n"                 \
    "  MOV TEMP[0], IMM[0].xxxx\n"                                     \
    "  MOV TEMP[1].x, -CONST[0].zzzz\n"                                \
    "  BGNLOOP\n"                                                      \
    "    SLT TEMP[2].x, CONST[0].zzzz, TEMP[1].xxxx\n"                 \
    "    IF TEMP[2].xxxx\n"                                            \
    "      BRK\n"                                                      \
    "    ENDIF\n"                                                      \
    "    MAD TEMP[3].xy, CONST[0].xyyy, TEMP[1].xxxx, IN[0].xyyy\n"    \
    "    MAX TEMP[3].xy, TEMP[3].xyyy, CONST[1].xyyy\n"                \
    "    MIN TEMP[3].xy, TEMP[3].xyyy, CONST[1].zwww\n"                \
    "    MUL TEMP[3].xy, TEMP[3].xyyy, CONST[2].xyyy\n"                \
    "    TEX TEMP[2], TEMP[3], SAMP[0], " target "\n"                  \
    "    ADD TEMP[0], TEMP[0], TEMP[2]\n"                              \
    "    ADD TEMP[1].x, TEMP[1].xxxx, IMM[0].yyyy\n"                   \
    "  ENDLOOP\n"                                                      \
    "  MUL TEMP[0], TEMP[0], CONST[0].wwww\n"                          \
    SDF("IN[1]", "CONST[3]", "CONST[4].xxxx")                          \
    "  ADD_SAT TEMP[2].x, IMM[0].zzzz, -TEMP[2].xxxx\n"                \
    "  MUL OUT[0], TEMP[0], TEMP[2].xxxx\n"                            \
    "  END\n"
static const char fs_blur_rect_text[] = FS_BLUR("RECT");
static const char fs_blur_2d_text[] = FS_BLUR("2D");

/* ---- the stream ------------------------------------------------------------------ */
static int flush_stream(void)
{
    if (S.overflow) { failed = 1; S.n = 0; S.overflow = 0; return -1; }
    if (!S.n) return 0;
    int rc = T->submit(S.buf, S.n);
    S.n = 0;
    if (rc) failed = 1;
    return rc;
}

/* Room for one more command group, submitting what is queued if need be. */
static void need(uint32_t dwords)
{
    if (!vg_room(&S, dwords)) flush_stream();
}

static uint32_t c255(uint32_t v) { return vg_f32_ratio((int64_t)(v & 255u), 255); }

/* ---- drawing state per target ---------------------------------------------------- */
/* How rows land. The renderer keeps a texture's row 0 at the bottom, where
 * GL has it, and a transfer moves the guest's row y to GL row y -- except in
 * a Y_0_TOP resource (QEMU makes every 2D resource so), whose transfers turn
 * the rows over, putting row y at h - 1 - y. Drawing must agree with the
 * transfers: into a Y_0_TOP target with the viewport upright (pixel row y on
 * GL row h - 1 - y), into any other with it flipped. The scissor reaches GL
 * as it is, in GL rows, so under an upright viewport it is mirrored too. */
static void use_target(uint32_t surface, uint32_t w, uint32_t h, int y0_top,
                       uint32_t sx0, uint32_t sy0, uint32_t sx1, uint32_t sy1)
{
    need(64);
    vg_framebuffer(&S, surface);
    vg_viewport(&S, w, h, !y0_top);
    if (y0_top) vg_scissor(&S, sx0, h - sy1, sx1, h - sy0);
    else        vg_scissor(&S, sx0, sy0, sx1, sy1);
    uint32_t c[4] = { vg_f32_ratio(2, (int64_t)w), vg_f32_ratio(-2, (int64_t)h), vg_f32(-1), vg_f32(1) };
    vg_constants(&S, VG_SHADER_VERTEX, c, 4);
}

static void use_screen(void)
{
    use_target(H_TARGET, target_w, target_h, target_y0_top, (uint32_t)damage.x, (uint32_t)damage.y,
               (uint32_t)(damage.x + damage.w), (uint32_t)(damage.y + damage.h));
}

/* A quad: dst in pixels, src in texels, frame-local coordinates from fx/fy.
 * Draws it now; returns 0, or -1 once the vertex buffer is full this frame. */
static int quad(const struct gpu_rect *dst, const struct gpu_rect *src, int32_t fx, int32_t fy)
{
    if (vb_off + QUAD_BYTES > VB_BYTES) {          /* a frame of hundreds of quads: start the buffer again */
        flush_stream();
        vb_off = 0;
    }
    uint32_t v[4 * 12];
    int32_t xs[2] = { dst->x, dst->x + dst->w }, ys[2] = { dst->y, dst->y + dst->h };
    int32_t us[2] = { src ? src->x : 0, src ? src->x + src->w : 0 };
    int32_t vs[2] = { src ? src->y : 0, src ? src->y + src->h : 0 };
    for (unsigned k = 0; k < 4; k++) {
        unsigned i = k & 1, j = k >> 1;
        uint32_t *p = v + k * 12;
        p[0] = vg_f32(xs[i]); p[1] = vg_f32(ys[j]); p[2] = 0; p[3] = vg_f32(1);
        p[4] = vg_f32(us[i]); p[5] = vg_f32(vs[j]); p[6] = 0; p[7] = 0;
        p[8] = vg_f32(xs[i] - fx); p[9] = vg_f32(ys[j] - fy); p[10] = 0; p[11] = 0;
    }
    need(12 + 48 + 13);
    vg_buffer_write(&S, vb_res, vb_off, v, QUAD_BYTES);
    vg_draw_strip(&S, vb_off / VERTEX_BYTES, 4);
    vb_off += QUAD_BYTES;
    return 0;
}

/* ---- images ------------------------------------------------------------------------- */
static int vc_image_create(void *pixels, uint32_t w, uint32_t h, uint32_t stride, uint32_t *id)
{
    if (!ready || failed || !w || !h || stride < w * 4 || (stride & 3)) return -1;
    unsigned i = 0;
    while (i < MAX_IMAGES && img[i].used) i++;
    if (i == MAX_IMAGES) return -1;
    uint32_t res = T->new_id();
    if (T->resource_create(res, VG_TEXTURE_RECT, VG_FORMAT_B8G8R8A8_UNORM, VG_BIND_SAMPLER_VIEW, w, h, 0,
                           pixels, (uint64_t)stride * (h - 1) + (uint64_t)w * 4)) return -1;
    flush_stream();                                   /* draws already queued go first */
    vg_sampler_view(&S, H_IMAGE + 2 * i, res, VG_FORMAT_B8G8R8A8_UNORM, VG_TEXTURE_RECT, 0);
    vg_sampler_view(&S, H_IMAGE + 2 * i + 1, res, VG_FORMAT_B8G8R8A8_UNORM, VG_TEXTURE_RECT, 1);
    if (flush_stream() || T->to_host(res, 0, 0, w, h, stride)) {
        T->resource_destroy(res);
        return -1;
    }
    img[i].used = 1; img[i].res = res; img[i].w = w; img[i].h = h; img[i].stride = stride;
    *id = i + 1;
    return 0;
}

static void vc_image_update(uint32_t id, const struct gpu_rect *r)
{
    if (!id || id > MAX_IMAGES || !img[id - 1].used || failed) return;
    int32_t x0 = r->x < 0 ? 0 : r->x, y0 = r->y < 0 ? 0 : r->y;
    int32_t x1 = r->x + r->w, y1 = r->y + r->h;
    if (x1 > (int32_t)img[id - 1].w) x1 = (int32_t)img[id - 1].w;
    if (y1 > (int32_t)img[id - 1].h) y1 = (int32_t)img[id - 1].h;
    if (x1 <= x0 || y1 <= y0) return;
    flush_stream();                                   /* draws that read the old pixels go first */
    if (T->to_host(img[id - 1].res, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0),
                   img[id - 1].stride)) failed = 1;
}

static void vc_image_destroy(uint32_t id)
{
    if (!id || id > MAX_IMAGES || !img[id - 1].used) return;
    unsigned i = id - 1;
    flush_stream();
    vg_destroy(&S, H_IMAGE + 2 * i, VG_OBJ_SAMPLER_VIEW);
    vg_destroy(&S, H_IMAGE + 2 * i + 1, VG_OBJ_SAMPLER_VIEW);
    flush_stream();
    T->resource_destroy(img[i].res);
    img[i].used = 0;
}

/* ---- a frame ------------------------------------------------------------------------ */
/* Draw into `res` (w x h): its surface and the view the blur reads it by are
 * made again whenever it is a different resource from last frame's. */
static int begin_on(uint32_t res, uint32_t w, uint32_t h, int y0_top, const struct gpu_rect *d)
{
    if (!ready || failed || !res || !w || !h) return -1;
    if (res != target_res || w != target_w || h != target_h) {
        need(32);
        if (target_res) {
            vg_destroy(&S, H_TARGET, VG_OBJ_SURFACE);
            vg_destroy(&S, H_TARGET_VIEW, VG_OBJ_SAMPLER_VIEW);
        }
        vg_surface(&S, H_TARGET, res, VG_FORMAT_B8G8R8X8_UNORM);
        vg_sampler_view(&S, H_TARGET_VIEW, res, VG_FORMAT_B8G8R8X8_UNORM, VG_TEXTURE_2D, 1);
        target_res = res; target_w = w; target_h = h;
    }
    target_y0_top = y0_top;
    damage = *d;
    if (damage.x < 0) { damage.w += damage.x; damage.x = 0; }
    if (damage.y < 0) { damage.h += damage.y; damage.y = 0; }
    if (damage.x + damage.w > (int32_t)w) damage.w = (int32_t)w - damage.x;
    if (damage.y + damage.h > (int32_t)h) damage.h = (int32_t)h - damage.y;
    if (damage.w <= 0 || damage.h <= 0) return -1;
    vb_off = 0;
    use_screen();
    return failed ? -1 : 0;
}

static int vc_begin(const struct gpu_rect *d)
{
    if (!ready || failed) return -1;
    uint32_t res, w, h;
    int y0_top;
    T->target(&res, &w, &h, &y0_top);
    return begin_on(res, w, h, y0_top, d);
}

static void vc_fill(const struct gpu_rect *r, uint32_t rgb)
{
    need(32);
    vg_bind(&S, H_BLEND_COPY, VG_OBJ_BLEND);
    vg_bind_shader(&S, H_FS_SOLID, VG_SHADER_FRAGMENT);
    uint32_t c[4] = { c255(rgb >> 16), c255(rgb >> 8), c255(rgb), vg_f32(1) };
    vg_constants(&S, VG_SHADER_FRAGMENT, c, 4);
    quad(r, 0, r->x, r->y);
}

/* The largest radius a w x h rectangle can round by. */
static uint32_t fit_radius(uint32_t radius, int32_t w, int32_t h)
{
    int32_t m = (w < h ? w : h) / 2;
    return (int32_t)radius > m ? (uint32_t)(m > 0 ? m : 0) : radius;
}

static void vc_shadow(const struct gpu_rect *frame, const struct gpu_rect *cover, uint32_t radius,
                      uint32_t width, uint32_t peak)
{
    if (!width || !peak || frame->w <= 0 || frame->h <= 0) return;
    radius = fit_radius(radius, frame->w, frame->h);
    /* No cover: one far enough away that nothing is under it. */
    struct gpu_rect none = { frame->x - 100000, frame->y - 100000, 1, 1 };
    const struct gpu_rect *c = cover && cover->w > 0 && cover->h > 0 ? cover : &none;
    need(40);
    vg_bind(&S, H_BLEND_OVER, VG_OBJ_BLEND);
    vg_bind_shader(&S, H_FS_SHADOW, VG_SHADER_FRAGMENT);
    uint32_t k[16] = { c255(peak), vg_f32((int64_t)radius), vg_f32_ratio(1, (int64_t)width), 0,
                       vg_f32_ratio(frame->w, 2), vg_f32_ratio(frame->h, 2), 0, 0,
                       vg_f32(c->x - frame->x), vg_f32(c->y - frame->y), 0, 0,
                       vg_f32_ratio(c->w, 2), vg_f32_ratio(c->h, 2), 0, 0 };
    vg_constants(&S, VG_SHADER_FRAGMENT, k, 16);
    struct gpu_rect q = { frame->x - (int32_t)width, frame->y - (int32_t)width,
                          frame->w + 2 * (int32_t)width, frame->h + 2 * (int32_t)width };
    quad(&q, 0, frame->x, frame->y);
}

static void vc_draw(const struct gpu_draw *d)
{
    if (!d->image || d->image > MAX_IMAGES || !img[d->image - 1].used) return;
    unsigned i = d->image - 1;
    struct gpu_rect frame = d->frame.w > 0 && d->frame.h > 0 ? d->frame : d->dst;
    uint32_t radius = fit_radius(d->radius, frame.w, frame.h);
    int plain = d->opaque && d->opacity == 255 && !radius && !d->ring;
    int scaled = d->src.w != d->dst.w || d->src.h != d->dst.h;
    need(48);
    vg_bind(&S, plain ? H_BLEND_COPY : H_BLEND_OVER, VG_OBJ_BLEND);
    vg_bind_shader(&S, H_FS_IMAGE, VG_SHADER_FRAGMENT);
    vg_sampler_views(&S, VG_SHADER_FRAGMENT, H_IMAGE + 2 * i + (d->opaque ? 1u : 0u));
    vg_bind_sampler(&S, VG_SHADER_FRAGMENT, scaled ? H_SAMP_LINEAR : H_SAMP_NEAREST);
    uint32_t ring = d->ring_rgb;
    uint32_t c[12] = {
        c255(d->opacity), vg_f32((int64_t)radius), d->ring ? vg_f32_ratio(2 * d->ring + 1, 2) : vg_f32(-1000), 0,
        vg_f32_ratio(frame.w, 2), vg_f32_ratio(frame.h, 2), 0, 0,
        d->ring ? c255(ring >> 16) : 0, d->ring ? c255(ring >> 8) : 0, d->ring ? c255(ring) : 0, d->ring ? vg_f32(1) : 0,
    };
    vg_constants(&S, VG_SHADER_FRAGMENT, c, 12);
    quad(&d->dst, &d->src, frame.x, frame.y);
}

/* A scratch texture the size of the screen, between the blur's passes. */
static int blur_scratch(void)
{
    if (blur_res && blur_w == target_w && blur_h == target_h) return 0;
    flush_stream();
    if (blur_res) {
        vg_destroy(&S, H_BLUR_SURF, VG_OBJ_SURFACE);
        vg_destroy(&S, H_BLUR_VIEW, VG_OBJ_SAMPLER_VIEW);
        flush_stream();
        T->resource_destroy(blur_res);
        blur_res = 0;
    }
    uint32_t res = T->new_id();
    if (T->resource_create(res, VG_TEXTURE_RECT, VG_FORMAT_B8G8R8A8_UNORM,
                           VG_BIND_SAMPLER_VIEW | VG_BIND_RENDER_TARGET, target_w, target_h, 0, 0, 0))
        return -1;
    blur_res = res;
    vg_surface(&S, H_BLUR_SURF, res, VG_FORMAT_B8G8R8A8_UNORM);
    vg_sampler_view(&S, H_BLUR_VIEW, res, VG_FORMAT_B8G8R8A8_UNORM, VG_TEXTURE_RECT, 0);
    blur_w = target_w; blur_h = target_h;
    return flush_stream();
}

/* One pass, sampling `view` through `shader` along x or y: the region is
 * texels [x0, x1) x [y0, y1) of a tw x th source (tw = th = 1: a rectangle
 * texture, addressed in texels already). Coverage comes from a rounded
 * frame `fw` x `fh` with corners `corner`, at the quad's frame origin. */
static void blur_pass(uint32_t view, uint32_t shader, int horizontal, uint32_t radius,
                      int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t tw, uint32_t th,
                      int32_t fw, int32_t fh, uint32_t corner)
{
    vg_bind(&S, horizontal ? H_BLEND_COPY : H_BLEND_OVER, VG_OBJ_BLEND);
    vg_bind_shader(&S, shader, VG_SHADER_FRAGMENT);
    vg_sampler_views(&S, VG_SHADER_FRAGMENT, view);
    vg_bind_sampler(&S, VG_SHADER_FRAGMENT, H_SAMP_NEAREST);
    uint32_t c[20] = {
        horizontal ? vg_f32(1) : 0, horizontal ? 0 : vg_f32(1), vg_f32((int64_t)radius),
        vg_f32_ratio(1, 2 * (int64_t)radius + 1),
        vg_f32_ratio(2 * (int64_t)x0 + 1, 2), vg_f32_ratio(2 * (int64_t)y0 + 1, 2),
        vg_f32_ratio(2 * (int64_t)x1 - 1, 2), vg_f32_ratio(2 * (int64_t)y1 - 1, 2),
        vg_f32_ratio(1, (int64_t)tw), vg_f32_ratio(1, (int64_t)th), 0, 0,
        vg_f32_ratio(fw, 2), vg_f32_ratio(fh, 2), 0, 0,
        vg_f32((int64_t)corner), 0, 0, 0,
    };
    vg_constants(&S, VG_SHADER_FRAGMENT, c, 20);
}

static void vc_blur(const struct gpu_rect *r0, uint32_t radius, const struct gpu_rect *frame, uint32_t corner)
{
    if (!radius || failed) return;
    struct gpu_rect r = *r0;
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > (int32_t)target_w) r.w = (int32_t)target_w - r.x;
    if (r.y + r.h > (int32_t)target_h) r.h = (int32_t)target_h - r.y;
    if (r.w <= 1 || r.h <= 1 || blur_scratch()) return;
    struct gpu_rect f = frame && frame->w > 0 && frame->h > 0 ? *frame : r;
    if (!frame) corner = 0;
    corner = fit_radius(corner, f.w, f.h);
    struct gpu_rect at0 = { 0, 0, r.w, r.h };
    /* Horizontal: the screen there -> the scratch texture's corner. The
     * screen is read where it keeps those rows: a Y_0_TOP one upside down
     * (see use_target), so its texture rows run from h - y downwards. */
    struct gpu_rect from = r;
    int32_t ty0 = r.y, ty1 = r.y + r.h;
    if (target_y0_top) {
        from.y = (int32_t)target_h - r.y; from.h = -r.h;
        ty0 = (int32_t)target_h - r.y - r.h; ty1 = (int32_t)target_h - r.y;
    }
    use_target(H_BLUR_SURF, blur_w, blur_h, 0, 0, 0, (uint32_t)r.w, (uint32_t)r.h);
    blur_pass(H_TARGET_VIEW, H_FS_BLUR_2D, 1, radius, r.x, ty0, r.x + r.w, ty1, target_w, target_h,
              r.w, r.h, 0);
    quad(&at0, &from, 0, 0);
    /* Vertical: scratch -> the screen, where it came from, cut to the frame. */
    use_screen();
    blur_pass(H_BLUR_VIEW, H_FS_BLUR_RECT, 0, radius, 0, 0, r.w, r.h, 1, 1, f.w, f.h, corner);
    quad(&r, &at0, f.x, f.y);
}

static int vc_end(void)
{
    if (failed) return -1;
    if (flush_stream()) return -1;
    if (probing) return 0;
    return T->present(target_res, (uint32_t)damage.x, (uint32_t)damage.y, (uint32_t)damage.w, (uint32_t)damage.h);
}

/* Only from the surface being shown, and only once a frame was drawn into
 * it: after a mode change the last frame's target may be a released surface,
 * whose pages are not ours to have written into any more. */
static int vc_read(const struct gpu_rect *r)
{
    if (!ready || failed || !target_res) return -1;
    if (!probing) {
        uint32_t res, w, h;
        int y0_top;
        T->target(&res, &w, &h, &y0_top);
        if (res != target_res || w != target_w || h != target_h) return -1;
    }
    flush_stream();
    return T->from_host(target_res, (uint32_t)r->x, (uint32_t)r->y, (uint32_t)r->w, (uint32_t)r->h, target_w * 4);
}

static const struct gpu_compose ops = {
    .name = "host GPU (virgl)",
    .image_create = vc_image_create, .image_update = vc_image_update, .image_destroy = vc_image_destroy,
    .begin = vc_begin, .fill = vc_fill, .shadow = vc_shadow, .blur = vc_blur, .draw = vc_draw,
    .end = vc_end, .read = vc_read,
};

/* ---- set-up ------------------------------------------------------------------------- */
/* A host that takes every command and draws something else -- an old
 * renderer, a GL driver that lacks what a shader assumes -- must not be the
 * desktop. So before the compositor is handed the ops, a frame is drawn into
 * a small target made the way QEMU makes a scanout (2D, Y_0_TOP) and read
 * back: a fill, an image's rows in order, opacity, and a blur that must come
 * back where it was taken from. */
#define PROBE_W 32
#define PROBE_H 16
static uint32_t probe_px[PROBE_W * PROBE_H], probe_img[4 * 4];

static int probe_near(uint32_t got, uint32_t want, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((got >> s) & 255u) - (int)((want >> s) & 255u);
        if (d < -tol || d > tol) return 0;
    }
    return 1;
}

static int selftest(void)
{
    uint32_t res = T->new_id();
    for (unsigned i = 0; i < PROBE_W * PROBE_H; i++) probe_px[i] = 0;
    if (T->resource_create(res, VG_TEXTURE_2D, VG_FORMAT_B8G8R8X8_UNORM,
                           VG_BIND_RENDER_TARGET | VG_BIND_SAMPLER_VIEW, PROBE_W, PROBE_H, Y_0_TOP,
                           probe_px, sizeof probe_px))
        return -1;
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++) probe_img[y * 4 + x] = 0xff000000u | (y * 0x40u << 16) | (x * 0x40u);
    uint32_t pic = 0;
    int ok = vc_image_create(probe_img, 4, 4, 16, &pic) == 0;
    probing = 1;
    struct gpu_rect all = { 0, 0, PROBE_W, PROBE_H };
    if (ok && begin_on(res, PROBE_W, PROBE_H, 1, &all) == 0) {
        vc_fill(&all, 0x000000);
        struct gpu_rect red = { 2, 2, 4, 3 };
        vc_fill(&red, 0xff0000);
        struct gpu_draw d = { pic, { 0, 0, 4, 4 }, { 8, 2, 4, 4 }, 1, 255, 0, 0, { 0, 0, 0, 0 }, 0 };
        vc_draw(&d);
        d.dst.x = 14; d.opacity = 128;
        vc_draw(&d);
        struct gpu_rect white = { 24, 2, 3, 4 };
        vc_fill(&white, 0xffffff);
        struct gpu_rect region = { 20, 0, 12, PROBE_H };
        vc_blur(&region, 1, 0, 0);
        ok = vc_end() == 0 && vc_read(&all) == 0;
    } else {
        ok = 0;
    }
    probing = 0;
#define P(x, y) (probe_px[(y) * PROBE_W + (x)] & 0xffffffu)
    ok = ok && P(2, 2) == 0xff0000 && P(5, 4) == 0xff0000 && P(1, 2) == 0 && P(2, 5) == 0 &&
         P(8, 2) == 0x000000 && P(9, 2) == 0x000040 && P(8, 5) == 0xc00000 &&       /* rows top-down */
         probe_near(P(15, 5), 0x600020, 2) &&                                         /* half of (0xc0, 0, 0x40) */
         /* the blur: a third of white one row (or column) off the block, and not turned over */
         probe_near(P(25, 1), 0x555555, 3) && probe_near(P(25, 3), 0xffffff, 3) &&
         probe_near(P(25, 6), 0x555555, 3) && probe_near(P(23, 3), 0x555555, 3) &&
         P(25, 7) == 0 && P(25, 10) == 0 && P(19, 3) == 0;
#undef P
    if (pic) vc_image_destroy(pic);
    flush_stream();
    vg_destroy(&S, H_TARGET, VG_OBJ_SURFACE);
    vg_destroy(&S, H_TARGET_VIEW, VG_OBJ_SAMPLER_VIEW);
    target_res = 0;                                  /* the next frame makes the scanout's */
    if (flush_stream()) ok = 0;
    T->resource_destroy(res);
    return ok ? 0 : -1;
}

int virgl_compose_init(const struct vgc_transport *t)
{
    T = t;
    ready = failed = probing = 0;
    memset(img, 0, sizeof img);
    blur_res = 0;
    blur_w = blur_h = 0;
    target_res = 0;
    S.n = 0; S.overflow = 0;
    vb_res = T->new_id();
    if (T->resource_create(vb_res, VG_BUFFER, VG_FORMAT_R8_UNORM, VG_BIND_VERTEX_BUFFER, VB_BYTES, 1, 0, 0, 0))
        return -1;
    vg_create_sub_ctx(&S, SUB_CTX);
    vg_set_sub_ctx(&S, SUB_CTX);
    vg_blend(&S, H_BLEND_OVER, 1);
    vg_blend(&S, H_BLEND_COPY, 0);
    vg_dsa_off(&S, H_DSA);
    vg_rasterizer(&S, H_RAST);
    vg_vertex_elements(&S, H_VE, 3);
    vg_sampler_state(&S, H_SAMP_NEAREST, 0);
    vg_sampler_state(&S, H_SAMP_LINEAR, 1);
    vg_shader(&S, H_VS, VG_SHADER_VERTEX, vs_text);
    vg_shader(&S, H_FS_IMAGE, VG_SHADER_FRAGMENT, fs_image_text);
    vg_shader(&S, H_FS_SHADOW, VG_SHADER_FRAGMENT, fs_shadow_text);
    vg_shader(&S, H_FS_SOLID, VG_SHADER_FRAGMENT, fs_solid_text);
    vg_shader(&S, H_FS_BLUR_RECT, VG_SHADER_FRAGMENT, fs_blur_rect_text);
    vg_shader(&S, H_FS_BLUR_2D, VG_SHADER_FRAGMENT, fs_blur_2d_text);
    vg_bind(&S, H_DSA, VG_OBJ_DSA);
    vg_bind(&S, H_RAST, VG_OBJ_RASTERIZER);
    vg_bind(&S, H_VE, VG_OBJ_VERTEX_ELEMENTS);
    vg_bind_shader(&S, H_VS, VG_SHADER_VERTEX);
    vg_vertex_buffer(&S, vb_res, VERTEX_BYTES);
    if (flush_stream()) return -1;
    ready = 1;
    if (selftest()) { ready = 0; return -1; }
    return 0;
}

const struct gpu_compose *virgl_compose(void)
{
    return ready && !failed ? &ops : 0;
}
