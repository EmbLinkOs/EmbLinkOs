/* soft_compose.h -- the operations of struct gpu_compose done by the CPU, to
 * the letter of the virgl shaders (drivers/video/virgl_compose.c): the same
 * rounded-rectangle distance for corners, ring and shadow, the same blending
 * of premultiplied pixels, the box blur fb_blur_region does. A backend whose
 * engine can only fill and copy (the Intel blitter) hands it the rest, and the
 * one suite of pixel checks (tests/drivers/compose_checks.h) holds both.
 *
 * Integer only: the kernel keeps no FPU state. Distances are in 1/256 px.
 * Pixels are 0xAARRGGBB words, premultiplied; every call draws only inside
 * `clip` and the surface. */
#ifndef EMBLINK_SOFT_COMPOSE_H
#define EMBLINK_SOFT_COMPOSE_H
#include <stdint.h>
#include "drivers/video/gpu.h"

struct sc_surface {
    uint8_t *px;               /* the first pixel */
    uint32_t stride;           /* bytes between rows */
    int32_t  w, h;
};

void sc_fill(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *r, uint32_t rgb);
/* An image as gpu_compose.draw draws it (the image is d->image's pixels). */
void sc_draw(const struct sc_surface *t, const struct gpu_rect *clip, const struct sc_surface *img,
             const struct gpu_draw *d);
void sc_shadow(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *frame,
               const struct gpu_rect *cover, uint32_t radius, uint32_t width, uint32_t peak);
/* `scratch` holds r's pixels once (r->w * r->h words). */
void sc_blur(const struct sc_surface *t, const struct gpu_rect *clip, const struct gpu_rect *r, uint32_t radius,
             const struct gpu_rect *frame, uint32_t corner, uint32_t *scratch);

/* A rectangle's intersection with another; 0 when nothing is left. */
int sc_intersect(struct gpu_rect *a, const struct gpu_rect *b);

#endif
