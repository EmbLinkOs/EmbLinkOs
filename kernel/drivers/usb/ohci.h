#ifndef __OHCI_H__
#define __OHCI_H__

#include "drivers/usb/usb.h"

// Bring up an OHCI (USB 1.x) controller and enumerate its root ports.
bool ohci_init_controller(struct usb_controller *ctrl);

/* Compare this controller's ports against what the device table remembers,
 * enumerating what appeared and tearing down what left. */
void ohci_rescan(void *hc);

#endif /* __OHCI_H__ */
