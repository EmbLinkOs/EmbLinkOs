#ifndef __BACKDROP_H__
#define __BACKDROP_H__

#include <stdint.h>

/* The blur behind a window that shows through (compositor.c's material).
 *
 * dst's [x0,x1) x [y0,y1) becomes the blur of src over the domain
 * [dx0,dx1) x [dy0,dy1) (the window's part on the screen), the domain's
 * edges repeated: the same pixels whether the rect is all of the domain or
 * one row of it, because the passes read past the rect. That is what lets
 * the compositor blur only where the backdrop changed, without a seam where
 * that part meets the rest. src and dst share `stride` (pixels), hold
 * 0x00RRGGBB, and are not the same buffer.
 *
 * Two box passes each way (horizontal, vertical, twice), each half the
 * radius: a tent, smoother than one box of the whole radius. Returns -1 if
 * scratch memory could not be had (dst untouched). */
int backdrop_blur(const uint32_t *src, uint32_t *dst, int stride, int dx0, int dy0, int dx1, int dy1,
                  int x0, int y0, int x1, int y1, int radius);
/* How far a blur of `radius` reads: what a changed pixel changes around it. */
int backdrop_reach(int radius);

#endif
