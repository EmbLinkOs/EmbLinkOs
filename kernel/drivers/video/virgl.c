/* virgl.c -- encoding the virgl command stream. See virgl.h. Layouts follow
 * virglrenderer's vrend_decode.c and its test encoder (testvirgl_encode.c). */
#include "drivers/video/virgl.h"

/* virgl_context_cmd */
#define CCMD_CREATE_OBJECT          1u
#define CCMD_BIND_OBJECT            2u
#define CCMD_DESTROY_OBJECT         3u
#define CCMD_SET_VIEWPORT_STATE     4u
#define CCMD_SET_FRAMEBUFFER_STATE  5u
#define CCMD_SET_VERTEX_BUFFERS     6u
#define CCMD_DRAW_VBO               8u
#define CCMD_RESOURCE_INLINE_WRITE  9u
#define CCMD_SET_SAMPLER_VIEWS      10u
#define CCMD_SET_CONSTANT_BUFFER    12u
#define CCMD_SET_SCISSOR_STATE      15u
#define CCMD_BIND_SAMPLER_STATES    18u
#define CCMD_SET_SUB_CTX            28u
#define CCMD_CREATE_SUB_CTX         29u
#define CCMD_BIND_SHADER            31u

#define CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))

/* Gallium values (p_defines.h, u_formats.h) */
#define BLENDFACTOR_ONE           1u
#define BLENDFACTOR_ZERO          0x11u
#define BLENDFACTOR_INV_SRC_ALPHA 0x13u
#define BLEND_ADD                 0u
#define MASK_RGBA                 0xfu
#define TEX_WRAP_CLAMP_TO_EDGE    2u
#define TEX_MIPFILTER_NONE        2u
#define TEX_FILTER_NEAREST        0u
#define TEX_FILTER_LINEAR         1u
#define SWIZZLE_X 0u
#define SWIZZLE_Y 1u
#define SWIZZLE_Z 2u
#define SWIZZLE_W 3u
#define SWIZZLE_1 5u

/* The quotient by long division, one bit a step, on 64-bit words only (a
 * freestanding kernel has no 128-bit divide). Both sides are first shifted
 * up to bit 63, so their ratio is in (1/2, 2) and 26 steps give the 24
 * significant bits, a guard bit and a sticky remainder. */
uint32_t vg_f32_ratio(int64_t num, int64_t den)
{
    if (!num || !den) return 0;
    uint32_t sign = 0;
    uint64_t n = (uint64_t)num, d = (uint64_t)den;
    if (num < 0) { sign ^= 1u; n = 0 - n; }
    if (den < 0) { sign ^= 1u; d = 0 - d; }
    int e = __builtin_clzll(d) - __builtin_clzll(n);  /* n/d = n'/d' * 2^e */
    n <<= __builtin_clzll(n);
    d <<= __builtin_clzll(d);
    uint64_t r = n, carry = 0;
    uint32_t q = 0;
    for (int i = 0; i < 26; i++) {                   /* q = floor(n'/d' * 2^25) */
        q <<= 1;
        if (carry || r >= d) { r -= d; q |= 1; }     /* with the carry, r + 2^64 - d < 2^64 */
        carry = r >> 63;
        r <<= 1;
    }
    uint32_t sticky = (r | carry) != 0, m, guard;
    if (q >> 25) { m = q >> 2; guard = (q >> 1) & 1; sticky |= q & 1; }   /* n' >= d' */
    else         { m = q >> 1; guard = q & 1; e--; }
    if (guard && (sticky || (m & 1))) m++;           /* round to nearest, ties to even */
    if (m >> 24) { m >>= 1; e++; }
    int biased = e + 127;
    if (biased <= 0) return sign << 31;              /* below the normal range: zero */
    if (biased >= 255) return (sign << 31) | 0x7f800000u;
    return (sign << 31) | ((uint32_t)biased << 23) | (m & 0x7fffffu);
}

int vg_room(const struct vg_stream *s, uint32_t dwords)
{
    return !s->overflow && s->n + dwords <= s->cap;
}

/* Reserve `n` dwords; NULL (and the stream marked unsendable) if they do not fit. */
static uint32_t *put(struct vg_stream *s, uint32_t n)
{
    if (!vg_room(s, n)) { s->overflow = 1; return 0; }
    uint32_t *p = s->buf + s->n;
    s->n += n;
    return p;
}

void vg_create_sub_ctx(struct vg_stream *s, uint32_t id)
{
    uint32_t *p = put(s, 2);
    if (p) { p[0] = CMD0(CCMD_CREATE_SUB_CTX, 0, 1); p[1] = id; }
}

void vg_set_sub_ctx(struct vg_stream *s, uint32_t id)
{
    uint32_t *p = put(s, 2);
    if (p) { p[0] = CMD0(CCMD_SET_SUB_CTX, 0, 1); p[1] = id; }
}

void vg_blend(struct vg_stream *s, uint32_t handle, int premultiplied)
{
    uint32_t *p = put(s, 1 + 11);                    /* VIRGL_OBJ_BLEND_SIZE: handle, S0, S1, 8 render targets */
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_BLEND, 11);
    p[1] = handle;
    p[2] = 0;                                        /* no independent blend, logicop, dither */
    p[3] = 0;
    uint32_t rt = MASK_RGBA << 27;
    if (premultiplied)
        rt |= 1u | (BLEND_ADD << 1) | (BLENDFACTOR_ONE << 4) | (BLENDFACTOR_INV_SRC_ALPHA << 9) |
              (BLEND_ADD << 14) | (BLENDFACTOR_ONE << 17) | (BLENDFACTOR_INV_SRC_ALPHA << 22);
    for (unsigned i = 0; i < 8; i++) p[4 + i] = i == 0 ? rt : 0;
}

void vg_dsa_off(struct vg_stream *s, uint32_t handle)
{
    uint32_t *p = put(s, 1 + 5);
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_DSA, 5);
    p[1] = handle;
    p[2] = p[3] = p[4] = p[5] = 0;                   /* no depth, no stencil, no alpha test; alpha ref 0.0 */
}

void vg_rasterizer(struct vg_stream *s, uint32_t handle)
{
    uint32_t *p = put(s, 1 + 9);
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_RASTERIZER, 9);
    p[1] = handle;
    p[2] = (1u << 1)          /* depth_clip */
         | (0u << 8)          /* cull_face: none */
         | (1u << 14)         /* scissor */
         | (1u << 29)         /* half_pixel_center */
         | (1u << 30);        /* bottom_edge_rule */
    p[3] = vg_f32(1);         /* point_size */
    p[4] = 0;                 /* sprite_coord_enable */
    p[5] = 0;                 /* S3: no stipple, no clip planes */
    p[6] = vg_f32(1);         /* line_width */
    p[7] = p[8] = p[9] = 0;   /* offsets */
}

void vg_vertex_elements(struct vg_stream *s, uint32_t handle, unsigned n)
{
    uint32_t *p = put(s, 2 + 4 * n);
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_VERTEX_ELEMENTS, 1 + 4 * n);
    p[1] = handle;
    for (unsigned i = 0; i < n; i++) {
        p[2 + i * 4] = i * 16u;                      /* src_offset */
        p[3 + i * 4] = 0;                            /* instance_divisor */
        p[4 + i * 4] = 0;                            /* vertex_buffer_index */
        p[5 + i * 4] = VG_FORMAT_R32G32B32A32_FLOAT;
    }
}

void vg_sampler_state(struct vg_stream *s, uint32_t handle, int linear)
{
    uint32_t *p = put(s, 1 + 9);
    if (!p) return;
    uint32_t f = linear ? TEX_FILTER_LINEAR : TEX_FILTER_NEAREST;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_SAMPLER_STATE, 9);
    p[1] = handle;
    p[2] = TEX_WRAP_CLAMP_TO_EDGE | (TEX_WRAP_CLAMP_TO_EDGE << 3) | (TEX_WRAP_CLAMP_TO_EDGE << 6) |
           (f << 9) | (TEX_MIPFILTER_NONE << 11) | (f << 13);
    p[3] = p[4] = p[5] = 0;                          /* lod bias, min/max lod */
    p[6] = p[7] = p[8] = p[9] = 0;                   /* border colour */
}

void vg_sampler_view(struct vg_stream *s, uint32_t handle, uint32_t res, uint32_t format,
                     uint32_t target, int opaque)
{
    uint32_t *p = put(s, 1 + 6);
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_SAMPLER_VIEW, 6);
    p[1] = handle;
    p[2] = res;
    p[3] = format | (target << 24);
    p[4] = 0;                                        /* first/last layer */
    p[5] = 0;                                        /* first/last level */
    p[6] = SWIZZLE_X | (SWIZZLE_Y << 3) | (SWIZZLE_Z << 6) | ((opaque ? SWIZZLE_1 : SWIZZLE_W) << 9);
}

void vg_surface(struct vg_stream *s, uint32_t handle, uint32_t res, uint32_t format)
{
    uint32_t *p = put(s, 1 + 5);
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_SURFACE, 5);
    p[1] = handle;
    p[2] = res;
    p[3] = format;
    p[4] = 0;                                        /* level */
    p[5] = 0;                                        /* first | last layer << 16 */
}

void vg_shader(struct vg_stream *s, uint32_t handle, uint32_t type, const char *tgsi)
{
    uint32_t len = 0;
    while (tgsi[len]) len++;
    len++;                                           /* the NUL travels too */
    uint32_t words = (len + 3) / 4;
    uint32_t *p = put(s, 1 + 5 + words);
    if (!p) return;
    p[0] = CMD0(CCMD_CREATE_OBJECT, VG_OBJ_SHADER, 5 + words);
    p[1] = handle;
    p[2] = type;
    p[3] = len;                                      /* first (and only) chunk: the full length */
    p[4] = len;                                      /* token budget: no TGSI token is under a character */
    p[5] = 0;                                        /* no stream output */
    uint8_t *text = (uint8_t *)(p + 6);
    for (uint32_t i = 0; i < words * 4; i++) text[i] = i < len ? (uint8_t)tgsi[i] : 0;
}

void vg_bind(struct vg_stream *s, uint32_t handle, uint32_t object)
{
    uint32_t *p = put(s, 2);
    if (p) { p[0] = CMD0(CCMD_BIND_OBJECT, object, 1); p[1] = handle; }
}

void vg_bind_shader(struct vg_stream *s, uint32_t handle, uint32_t type)
{
    uint32_t *p = put(s, 3);
    if (p) { p[0] = CMD0(CCMD_BIND_SHADER, 0, 2); p[1] = handle; p[2] = type; }
}

void vg_destroy(struct vg_stream *s, uint32_t handle, uint32_t object)
{
    uint32_t *p = put(s, 2);
    if (p) { p[0] = CMD0(CCMD_DESTROY_OBJECT, object, 1); p[1] = handle; }
}

void vg_framebuffer(struct vg_stream *s, uint32_t surface)
{
    uint32_t *p = put(s, 4);
    if (p) { p[0] = CMD0(CCMD_SET_FRAMEBUFFER_STATE, 0, 3); p[1] = 1; p[2] = 0; p[3] = surface; }
}

void vg_viewport(struct vg_stream *s, uint32_t w, uint32_t h, int flip)
{
    /* The vertex shader hands over clip coordinates made from pixels by its
     * constants (see the compositor's shaders); this maps them back:
     * window = ndc * scale + translate. */
    uint32_t *p = put(s, 8);
    if (!p) return;
    p[0] = CMD0(CCMD_SET_VIEWPORT_STATE, 0, 7);
    p[1] = 0;                                        /* start slot */
    p[2] = vg_f32_ratio((int64_t)w, 2);
    p[3] = vg_f32_ratio(flip ? -(int64_t)h : (int64_t)h, 2);
    p[4] = vg_f32_ratio(1, 2);
    p[5] = vg_f32_ratio((int64_t)w, 2);
    p[6] = vg_f32_ratio((int64_t)h, 2);
    p[7] = vg_f32_ratio(1, 2);
}

void vg_scissor(struct vg_stream *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint32_t *p = put(s, 4);
    if (!p) return;
    p[0] = CMD0(CCMD_SET_SCISSOR_STATE, 0, 3);
    p[1] = 0;
    p[2] = (x0 & 0xffffu) | ((y0 & 0xffffu) << 16);
    p[3] = (x1 & 0xffffu) | ((y1 & 0xffffu) << 16);
}

void vg_vertex_buffer(struct vg_stream *s, uint32_t res, uint32_t stride)
{
    uint32_t *p = put(s, 4);
    if (p) { p[0] = CMD0(CCMD_SET_VERTEX_BUFFERS, 0, 3); p[1] = stride; p[2] = 0; p[3] = res; }
}

void vg_sampler_views(struct vg_stream *s, uint32_t type, uint32_t view)
{
    uint32_t *p = put(s, 4);
    if (p) { p[0] = CMD0(CCMD_SET_SAMPLER_VIEWS, 0, 3); p[1] = type; p[2] = 0; p[3] = view; }
}

void vg_bind_sampler(struct vg_stream *s, uint32_t type, uint32_t state)
{
    uint32_t *p = put(s, 4);
    if (p) { p[0] = CMD0(CCMD_BIND_SAMPLER_STATES, 0, 3); p[1] = type; p[2] = 0; p[3] = state; }
}

void vg_constants(struct vg_stream *s, uint32_t type, const uint32_t *dwords, unsigned n)
{
    uint32_t *p = put(s, 3 + n);
    if (!p) return;
    p[0] = CMD0(CCMD_SET_CONSTANT_BUFFER, 0, 2 + n);
    p[1] = type;
    p[2] = 0;                                        /* index: unused by the host */
    for (unsigned i = 0; i < n; i++) p[3 + i] = dwords[i];
}

void vg_buffer_write(struct vg_stream *s, uint32_t res, uint32_t offset, const void *data, uint32_t bytes)
{
    uint32_t words = (bytes + 3) / 4;
    uint32_t *p = put(s, 12 + words);
    if (!p) return;
    p[0] = CMD0(CCMD_RESOURCE_INLINE_WRITE, 0, 11 + words);
    p[1] = res;
    p[2] = 0;                                        /* level */
    p[3] = 0;                                        /* usage */
    p[4] = 0;                                        /* stride */
    p[5] = 0;                                        /* layer stride */
    p[6] = offset;                                   /* box: x is the byte offset in a buffer */
    p[7] = 0;
    p[8] = 0;
    p[9] = bytes;
    p[10] = 1;
    p[11] = 1;
    const uint8_t *src = (const uint8_t *)data;
    uint8_t *dst = (uint8_t *)(p + 12);
    for (uint32_t i = 0; i < words * 4; i++) dst[i] = i < bytes ? src[i] : 0;
}

void vg_draw_strip(struct vg_stream *s, uint32_t start, uint32_t count)
{
    uint32_t *p = put(s, 13);
    if (!p) return;
    p[0] = CMD0(CCMD_DRAW_VBO, 0, 12);
    p[1] = start;
    p[2] = count;
    p[3] = VG_PRIM_TRIANGLE_STRIP;
    p[4] = 0;                                        /* not indexed */
    p[5] = 1;                                        /* one instance */
    p[6] = 0;                                        /* index bias */
    p[7] = 0;                                        /* start instance */
    p[8] = 0;                                        /* primitive restart */
    p[9] = 0;                                        /* restart index */
    p[10] = start;                                   /* min index */
    p[11] = start + count - 1;                       /* max index */
    p[12] = 0;                                       /* count from stream output */
}

