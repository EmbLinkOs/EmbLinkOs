#ifndef __I8042_H__
#define __I8042_H__

#include <stdint.h>
#include "include/types.h"

/* kernel/drivers/input/i8042.h -- the PS/2 controller, brought up ONCE.
 *
 * The keyboard driver and the mouse driver both talk through this one chip,
 * and until this file existed they each configured it themselves: two
 * read-modify-writes of the same config byte, neither aware of the other,
 * neither flushing the output buffer first. On QEMU that works because the
 * buffer is always empty when the kernel starts. On a laptop it is not: UEFI
 * runs its own keyboard driver right up to ExitBootServices, and a scancode
 * left sitting in the output buffer is read back AS THE CONFIG BYTE and
 * written straight back into the controller -- which is how a machine ends up
 * with an internal keyboard that never reports anything.
 *
 * So the controller gets one owner, and the owner does what the hardware
 * actually asks for: quiet both ports, empty the buffer, self-test, TEST each
 * port, and only then enable what is really there. What is really there is
 * the other half of this: a laptop whose touchpad is on I2C has no second
 * port, and pretending otherwise costs a stall on every unanswered command.
 */

/* Bring the controller up. Idempotent: the first caller does the work, later
 * callers get the same answer. False means there is no usable 8042 -- the
 * firmware said so, or it failed its own self-test -- and in that case no
 * port has been touched and the keyboard/mouse drivers should stand down. */
bool i8042_init(void);

bool i8042_have_kbd(void);   /* port 1 tested good and a device answered */
bool i8042_have_aux(void);   /* port 2 exists, tested good, device answered */

/* Read one byte from the data port, waiting up to `spins` polls of the status
 * register. -1 on timeout. Never used from an IRQ handler: the handler reads
 * 0x60 directly because the byte is already there. */
int  i8042_read(uint32_t spins);

/* Drain whatever is sitting in the output buffer. Returns how many bytes went. */
uint32_t i8042_flush(void);

bool i8042_cmd(uint8_t cmd);                     /* controller command byte     */
bool i8042_cmd_data(uint8_t cmd, uint8_t data);  /* command + one argument byte */
int  i8042_cmd_read(uint8_t cmd);                /* command, then its reply     */

/* Send a byte to the device on port 1 (keyboard) or port 2 (aux), retrying a
 * RESEND and consuming the ACK. False means no ACK -- nothing is there, or it
 * is not listening. */
bool i8042_kbd_write(uint8_t byte);
bool i8042_aux_write(uint8_t byte);

/* Turn the two interrupt sources on in the config byte, for the ports that
 * exist. Called by the drivers once their handlers are registered, so a byte
 * cannot arrive before there is somewhere to put it. */
void i8042_irq_enable(bool kbd, bool aux);

#endif /* __I8042_H__ */
