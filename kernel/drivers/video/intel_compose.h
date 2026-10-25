/* intel_compose.h -- the desktop composed with the Gen9 blitter.
 *
 * struct gpu_compose on an engine that can fill and copy and nothing else:
 * frames are drawn into a composition buffer the blitter and the CPU both
 * reach (system pages, in the LLC, mapped into the blitter's PPGTT), and
 * end() has the blitter copy the damage onto the scanout. What is a plain
 * copy goes to the blitter -- the wallpaper, every opaque window's inside and
 * title bar, the straight runs of its border, fills, the present itself;
 * what blends goes to the CPU (soft_compose.c) -- rounded corners, shadows,
 * glass, translucent windows, scaled ghosts. Images are the windows' own
 * pages, so nothing is ever uploaded: the blitter reads what the app drew.
 *
 * Everything outside -- which surface is on screen, memory, the backbuffer
 * screen reads go to -- comes through intel_compose_env, so the model test
 * (tests/drivers/intel_compose_test.c) runs all of it on a simulated engine. */
#ifndef EMBLINK_INTEL_COMPOSE_H
#define EMBLINK_INTEL_COMPOSE_H
#include <stdint.h>
#include "drivers/video/gpu.h"
#include "drivers/video/intel_blt.h"

struct intel_compose_env {
    struct intel_blt *blt;
    const struct intel_gpu_mem *mem;
    /* The surface on screen: its GGTT address, pitch and size; -1: none. */
    int   (*front)(void *ctx, uint32_t *ggtt, uint32_t *pitch, uint32_t *w, uint32_t *h);
    /* The framebuffer's draw buffer, where read() leaves the screen's pixels. */
    void *(*backbuffer)(void *ctx, uint32_t *pitch);
    /* Page-aligned memory the CPU and, through va_phys, the blitter reach;
     * *raw is what free takes back. */
    void *(*alloc)(void *ctx, uint64_t bytes, void **raw);
    void  (*free)(void *ctx, void *raw);
    void  *ctx;
};

int intel_compose_init(const struct intel_compose_env *env);
/* The ops once initialised, NULL before or when the blitter is gone. */
const struct gpu_compose *intel_compose(void);
/* A scanout at `ggtt` is about to be released: stop writing it. */
void intel_compose_forget_front(uint32_t ggtt);

#endif
