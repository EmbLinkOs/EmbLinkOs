/* virgl.h -- the virgl command stream a virtio-gpu 3D context runs.
 *
 * With VIRTIO_GPU_F_VIRGL the host renders for us: a context receives Gallium
 * state and draw commands (SUBMIT_3D) and virglrenderer turns them into
 * OpenGL on the host's GPU. This file only ENCODES: dword streams laid out as
 * virglrenderer's vrend_decode.c parses them (virgl_protocol.h), with the
 * Gallium enum values of p_defines.h. No device access, so it is
 * host-tested (tests/drivers/virgl_encode_test.c).
 *
 * The kernel has no FPU state (-mno-sse), so every float the GPU needs is
 * made here as IEEE-754 bits from integers: vg_f32_ratio. */
#ifndef EMBLINK_VIRGL_H
#define EMBLINK_VIRGL_H
#include <stdint.h>

/* Object types (virgl_object_type). */
#define VG_OBJ_BLEND           1u
#define VG_OBJ_RASTERIZER      2u
#define VG_OBJ_DSA             3u
#define VG_OBJ_SHADER          4u
#define VG_OBJ_VERTEX_ELEMENTS 5u
#define VG_OBJ_SAMPLER_VIEW    6u
#define VG_OBJ_SAMPLER_STATE   7u
#define VG_OBJ_SURFACE         8u

/* Gallium values. */
#define VG_SHADER_VERTEX    0u
#define VG_SHADER_FRAGMENT  1u
#define VG_TEXTURE_2D       2u
#define VG_TEXTURE_RECT     5u
#define VG_BUFFER           0u
#define VG_PRIM_TRIANGLE_STRIP 5u
#define VG_FORMAT_B8G8R8A8_UNORM 1u
#define VG_FORMAT_B8G8R8X8_UNORM 2u
#define VG_FORMAT_R8_UNORM       64u
#define VG_FORMAT_R32G32B32A32_FLOAT 31u
#define VG_BIND_RENDER_TARGET  (1u << 1)
#define VG_BIND_SAMPLER_VIEW   (1u << 3)
#define VG_BIND_VERTEX_BUFFER  (1u << 4)

#define VG_STREAM_MAX_DWORDS 16384u   /* 64 KiB: one SUBMIT_3D */

struct vg_stream {
    uint32_t *buf;
    uint32_t n, cap;
    int overflow;                      /* a command did not fit: the stream is not sendable */
};

/* num/den as an IEEE-754 single, round to nearest, without floating point. */
uint32_t vg_f32_ratio(int64_t num, int64_t den);
static inline uint32_t vg_f32(int64_t v) { return vg_f32_ratio(v, 1); }

/* Room for `dwords` more (header included)? */
int vg_room(const struct vg_stream *s, uint32_t dwords);

/* Sub-contexts: every object below lives in the current one. */
void vg_create_sub_ctx(struct vg_stream *s, uint32_t id);
void vg_set_sub_ctx(struct vg_stream *s, uint32_t id);

/* State objects. premultiplied: dst = src + dst * (1 - src.a); else no blending. */
void vg_blend(struct vg_stream *s, uint32_t handle, int premultiplied);
void vg_dsa_off(struct vg_stream *s, uint32_t handle);
/* Triangles drawn whatever their winding, pixel centres at .5, the scissor on. */
void vg_rasterizer(struct vg_stream *s, uint32_t handle);
/* `n` attributes of four floats each, packed in one vertex buffer. */
void vg_vertex_elements(struct vg_stream *s, uint32_t handle, unsigned n);
void vg_sampler_state(struct vg_stream *s, uint32_t handle, int linear);
/* A view of a texture; opaque: alpha reads as 1 whatever the pixels hold. */
void vg_sampler_view(struct vg_stream *s, uint32_t handle, uint32_t res, uint32_t format,
                     uint32_t target, int opaque);
void vg_surface(struct vg_stream *s, uint32_t handle, uint32_t res, uint32_t format);
/* A shader from TGSI text. */
void vg_shader(struct vg_stream *s, uint32_t handle, uint32_t type, const char *tgsi);
void vg_bind(struct vg_stream *s, uint32_t handle, uint32_t object);
void vg_bind_shader(struct vg_stream *s, uint32_t handle, uint32_t type);
void vg_destroy(struct vg_stream *s, uint32_t handle, uint32_t object);

/* Per-draw state. */
void vg_framebuffer(struct vg_stream *s, uint32_t surface);
/* Pixel coordinates in, pixels out: a w x h target, y down. flip: pixel row
 * y on GL row y (a plain texture); else on GL row h - 1 - y (a Y_0_TOP one). */
void vg_viewport(struct vg_stream *s, uint32_t w, uint32_t h, int flip);
void vg_scissor(struct vg_stream *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
void vg_vertex_buffer(struct vg_stream *s, uint32_t res, uint32_t stride);
void vg_sampler_views(struct vg_stream *s, uint32_t type, uint32_t view);
void vg_bind_sampler(struct vg_stream *s, uint32_t type, uint32_t state);
/* Constant vec4s for a stage: n dwords, already float bits. */
void vg_constants(struct vg_stream *s, uint32_t type, const uint32_t *dwords, unsigned n);
/* Bytes into a buffer resource, in the stream. */
void vg_buffer_write(struct vg_stream *s, uint32_t res, uint32_t offset, const void *data, uint32_t bytes);
void vg_draw_strip(struct vg_stream *s, uint32_t start, uint32_t count);

#endif
