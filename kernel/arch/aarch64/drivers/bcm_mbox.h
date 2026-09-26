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

/* A clock's rate in Hz, from the firmware (which owns every clock on a Pi),
 * or 0 if there is no mailbox or it would not say. Clock IDs are the
 * firmware's: 12 is EMMC2, the Pi 4's SD slot; 1 the older EMMC controller. */
#define BCM_CLOCK_EMMC  1
#define BCM_CLOCK_EMMC2 12
uint32_t bcm_mbox_clock_rate(uint32_t clock_id);

#endif
