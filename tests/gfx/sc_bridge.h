/* sc_bridge.h -- see sc_bridge.c */
#ifndef SC_BRIDGE_H
#define SC_BRIDGE_H
#include <stdint.h>
/* sc_draw of the whole of `img` (sw x sh, premultiplied) into the whole of
 * `dst` (dw x dh), at `opacity`: not opaque, square corners, no ring */
void sc_bridge_draw(uint32_t *dst, int dw, int dh, const uint32_t *img, int sw, int sh, uint32_t opacity);
#endif
