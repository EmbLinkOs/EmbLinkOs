/* A network card on the end of a USB cable -- the escape hatch that gets a
 * machine online when its wireless chip has no driver. See the .c. */
#ifndef _EMBK_USB_NET_H_
#define _EMBK_USB_NET_H_

#include <stdint.h>
#include "include/types.h"

struct usb_device;

/* Called from the USB class dispatch when a network interface is enumerated.
 * True if this kernel brought it up. */
bool usb_net_attach(struct usb_device *dev);

/* The net_driver operations, for net.c's table. Declared rather than exported
 * as a struct so the table stays in one place beside e1000 and rtl8139. */
bool usb_net_drv_init(uint8_t mac_out[6]);
int  usb_net_drv_tx(const void *frame, uint32_t len);
void usb_net_drv_poll(void);
int  usb_net_drv_link(void);
bool usb_net_present(void);
void usb_net_stats(uint64_t *rx, uint64_t *tx, uint64_t *dropped);

#endif
