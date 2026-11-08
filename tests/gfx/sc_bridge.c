/* sc_bridge.c -- soft_compose's sc_draw behind plain types, for pix_test.
 *
 * The kernel's types.h defines size_t itself, which a hosted <stdio.h> also
 * does, differently; this file includes only the kernel's side, and pix_test
 * only the host's. */
#include "gfx/soft_compose.h"
#include "tests/gfx/sc_bridge.h"

void sc_bridge_draw(uint32_t *dst, int dw, int dh, const uint32_t *img, int sw, int sh, uint32_t opacity)
{
    struct sc_surface t = { (uint8_t *)dst, (uint32_t)dw * 4, dw, dh };
    struct sc_surface im = { (uint8_t *)(uintptr_t)img, (uint32_t)sw * 4, sw, sh };
    struct gpu_rect clip = { 0, 0, dw, dh };
    struct gpu_draw d = { 0 };
    d.src.w = sw; d.src.h = sh; d.dst.w = dw; d.dst.h = dh;
    d.opacity = (uint8_t)opacity;
    sc_draw(&t, &clip, &im, &d);
}
