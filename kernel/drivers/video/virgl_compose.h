/* virgl_compose.h -- the desktop composed on the host's GPU through virgl.
 *
 * Implements struct gpu_compose (gpu.h) as virgl command streams: windows are
 * rectangle textures backed by their own pages, drawn as quads with
 * premultiplied blending; rounded corners, the border ring and the drop
 * shadow come from a rounded-rectangle distance in the fragment shaders; the
 * glass blur is two box-blur passes, the screen into a scratch texture and
 * back.
 *
 * It does not touch the device itself. A transport creates resources, moves
 * pixels and submits streams: the kernel's is virtio-gpu (virtio_gpu.c); the
 * host test's is libvirglrenderer directly (tests/drivers/virgl_compose_test.c),
 * which is how every shader and coordinate here is checked against a real
 * virglrenderer and Mesa without booting anything. */
#ifndef EMBLINK_VIRGL_COMPOSE_H
#define EMBLINK_VIRGL_COMPOSE_H
#include <stdint.h>
#include "drivers/video/gpu.h"

struct vgc_transport {
    uint32_t (*new_id)(void);
    /* A resource, attached to the 3D context; `backing` (bytes long) is
     * where transfers read and write its pixels, or NULL for none. flags:
     * VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP (1) or 0. */
    int  (*resource_create)(uint32_t id, uint32_t target, uint32_t format, uint32_t bind,
                            uint32_t w, uint32_t h, uint32_t flags, void *backing, uint64_t bytes);
    void (*resource_destroy)(uint32_t id);
    /* Backing -> resource, and back, for a rectangle (rows `stride` bytes apart). */
    int  (*to_host)(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride);
    int  (*from_host)(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride);
    int  (*submit)(const uint32_t *dwords, uint32_t n);
    /* The scanout resource frames are drawn into, its size, and whether it
     * was made Y_0_TOP (its transfers turn rows over; see use_target). */
    void (*target)(uint32_t *id, uint32_t *w, uint32_t *h, int *y0_top);
    /* Show a rectangle of the target. */
    int  (*present)(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
};

/* Make the context's state, shaders and vertex buffer, then draw a probe
 * frame and read it back. 0, or -1 when the host refused any of it or drew
 * the probe wrong (composition then stays on the CPU). */
int virgl_compose_init(const struct vgc_transport *t);

/* The ops, once initialised; NULL before, or after a failure. */
const struct gpu_compose *virgl_compose(void);

#endif
