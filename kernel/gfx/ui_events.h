#ifndef __GFX_UI_EVENTS_H__
#define __GFX_UI_EVENTS_H__

/* kernel/gfx/ui_events.h -- something an interface might answer happened.
 *
 * An application with nothing to draw used to find out whether that was still
 * true by asking every 5 ms: sleep, poll the pointer, poll the keyboard, sleep
 * again. Six windows on a still desktop woke the machine twelve hundred times
 * a second to learn that nothing had happened, and a click waited up to 5 ms
 * to be noticed.
 *
 * Now the things an interface answers -- a key, the pointer moving over or
 * clicking a window, focus changing, a window asked to close -- bump one
 * sequence number, and an application waits for it to change (or for its own
 * next deadline: a clock to redraw, a display to re-check). The sequence says
 * only THAT something happened; the application polls its input as it always
 * did to learn what, so a wake meant for another window costs one poll. */

#include <stdint.h>

/* Register the wait queue with the scheduler's timer drain. Once, before the
 * first wait; until then a post just counts. */
void ui_events_init(void);

/* Something happened. Any context, the keyboard interrupt included -- but
 * never while holding the scheduler lock or the compositor's. */
void ui_event_post(void);

/* Wait until the sequence differs from *seq or `timeout_ms` passes. Returns 1
 * with *seq updated when it moved, 0 on timeout, -EMBK_ECANCELED if the
 * process was told to stop. A timeout of 0 only looks. */
int ui_event_wait(uint32_t *seq, uint32_t timeout_ms);

#endif
