#ifndef _KPRINTF_H_
#define _KPRINTF_H_


#include "include/types.h"   /* bool */
#include <stdint.h>


void kprintf(const char *fmt, ...); // variadic function

/* Redirect kernel log output to something better than the serial port once it
 * exists -- the framebuffer console registers itself here from console_init().
 *
 * `ready` is consulted on EVERY character rather than latched, so a subsystem
 * that goes away again (or has not finished initialising) falls back to serial
 * instead of writing into a half-built console. Passing (0, 0) restores serial.
 *
 * This is a registration, not a call-out, so that the log path -- which has to
 * work when everything else is broken -- depends on nothing but the UART. */
void kprintf_set_secondary(bool (*ready)(void), void (*put)(char));
int  snprintf(char *buffer, size_t size, const char *fmt, ...);

#endif /* _KPRINTF_H_ */