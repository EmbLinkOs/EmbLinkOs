#ifndef _VIRTIO_INPUT_H_
#define _VIRTIO_INPUT_H_

#include "include/types.h"

/* virtio-input: the keyboard and pointer on a machine with no PS/2 controller.
 * See virtio_input.c -- this is a translation table into the SHARED keyboard
 * and mouse state, not a second input stack. */

/* Probe every virtio-input function on the bus, identify each by capability
 * (keyboard vs tablet) and arm its event queue. Safe to call with no such
 * device: it says so and does nothing. */
void virtio_input_init(void);

/* Drain pending events into the shared keyboard/mouse state. Polled, so this
 * must be called from schedulable context on a regular cadence -- the
 * compositor tick is exactly right, and is where it is called from. */
void virtio_input_poll(void);

bool virtio_input_present(void);

/* How many events have actually ARRIVED, per class. The boot self-test asserts
 * on these: a device that enumerates but never delivers is indistinguishable
 * from a working one until a key is pressed, and that is precisely the failure
 * worth catching at boot rather than by hand. */
void virtio_input_stats(uint32_t *keys, uint32_t *pointer);

#endif /* _VIRTIO_INPUT_H_ */
