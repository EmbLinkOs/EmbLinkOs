#ifndef __UHCI_H__
#define __UHCI_H__

#include "drivers/usb/usb.h"

// Bring up a UHCI (USB 1.x) controller and enumerate its root ports.
bool uhci_init_controller(struct usb_controller *ctrl);

/* Compare this controller's ports against what the device table remembers,
 * enumerating what appeared and tearing down what left. */
void uhci_rescan(void *hc);

#endif /* __UHCI_H__ */
