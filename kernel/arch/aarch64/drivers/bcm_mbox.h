#ifndef ARCH_AARCH64_BCM_MBOX_H
#define ARCH_AARCH64_BCM_MBOX_H

#include "boot/boot_protocol.h"

/* The Raspberry Pi's VideoCore mailbox -- docs/RPI4.md P0b.
 *
 * Asks the firmware for a 32-bpp framebuffer on the attached display and, if
 * it gets one, records it in the boot protocol's fb_* fields, which is where
 * rawcon (the early kernel log on screen) and fb_init (the desktop) look on
 * every machine. Silent and harmless on a machine without the mailbox.
 *
 * Runs from boot_protocol_capture(), before pmm exists: it reaches the
 * mailbox through boot.S's static MMIO window and uses a message buffer in
 * .bss, so it needs no allocator and no runtime mapping. */
void bcm_fb_probe(struct boot_protocol *proto);

#endif
