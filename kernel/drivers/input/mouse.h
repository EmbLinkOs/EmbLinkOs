#ifndef _MOUSE_H_
#define _MOUSE_H_

#include <stdint.h>

/* PS/2 mouse driver. Tracks an absolute cursor position (clamped to the screen)
 * plus a button bitmask, updated from IRQ12 packets. */

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

/* Set the clamp bounds + start the cursor at the centre, then bring the PS/2
 * aux device up (enable, IRQ12, data reporting) and register the handler. */
void mouse_init(uint32_t screen_w, uint32_t screen_h);

/* Current cursor position + button bitmask (a snapshot; safe to poll). */
void mouse_get_state(int32_t *x, int32_t *y, uint32_t *buttons);
int32_t mouse_take_wheel(void);   /* consume accumulated scroll delta (+up/-down) */
/* Take the oldest LEFT-button press not yet taken, with the millisecond it
 * HAPPENED; 1 if there was one, 0 if not. Call it until it returns 0.
 *
 * An event, not a level, because a level cannot be counted: a whole
 * down-up-down-up between two polls leaves the state where it started and a
 * caller diffing it sees nothing at all -- which is what made a fast
 * double-click impossible, the second click being lost here in the driver
 * before anything else could have a view.
 *
 * And it carries its own TIME because the caller's is the wrong one: two
 * presses noticed on different ticks would otherwise look as far apart as the
 * ticks are, which on a busy kernel loop is wider than any double-click
 * window. */
int mouse_take_press(uint32_t *when_ms);

/* Set the cursor from an ABSOLUTE pointing device (e.g. a USB tablet): x/y are
 * in [0, range], scaled to the screen. Unlike the relative PS/2 path this maps
 * the host cursor 1:1 and never "escapes" the window. buttons uses MOUSE_BTN_*;
 * wheel is a relative delta to accumulate. */
void mouse_set_absolute(int32_t x, int32_t y, int32_t range,
                        uint32_t buttons, int32_t wheel);

#endif /* _MOUSE_H_ */
