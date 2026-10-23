/* kernel/drivers/i2c/smbus.h -- the chipset's two-wire bus.
 *
 * On a real machine this reaches the battery, the memory modules' SPD, the
 * temperature sensors, and a monitor's EDID through its DDC lines.
 *
 * IT DOES NOT REACH A LAPTOP TOUCHPAD. That is I2C-HID on an Intel LPSS or
 * AMD designware controller, which is a different piece of hardware entirely
 * and which QEMU does not emulate -- so that driver can only be written
 * against the target machine. See docs/HARDWARE_GAPS.md.
 */
#ifndef _EMBK_SMBUS_H_
#define _EMBK_SMBUS_H_

#include <stdint.h>
#include "include/types.h"

/* Find and enable the chipset's SMBus host. After pci_init. False means this
 * machine has none this kernel knows, which is not an error. */
bool smbus_init(void);
bool smbus_present(void);
const char *smbus_host_name(void);

/* Is anything at this address? A quick-write: the only probe with no side
 * effect on whatever happens to be there. */
bool smbus_probe(uint8_t addr);

bool smbus_read_byte_data(uint8_t addr, uint8_t cmd, uint8_t *out);
bool smbus_write_byte_data(uint8_t addr, uint8_t cmd, uint8_t val);
bool smbus_read_word_data(uint8_t addr, uint8_t cmd, uint16_t *out);

/* ---- what is actually on the bus ---------------------------------------- */
#define EDID_I2C_ADDR    0x50   /* a monitor, over its DDC lines */
#define SPD_I2C_ADDR_0   0x50   /* memory module 0 -- the SAME address, on a
                                 * different bus segment on real hardware */
#define SMART_BATTERY_ADDR 0x0B /* a Smart Battery answers here directly */

/* Read a monitor's 128-byte EDID. Verifies the fixed header AND the checksum,
 * because a bus that answered with floating bytes looks exactly like a
 * monitor that answered. */
bool smbus_read_edid(uint8_t *out128);

/* As above, from a given address. A monitor is at 0x50 on a real machine's
 * DDC lines -- which on a PC is the SAME address as the first memory module's
 * SPD, on a different bus segment. A test rig can put one elsewhere. */
bool smbus_read_edid_at(uint8_t addr, uint8_t *out128);

/* Decode who made it. `vendor` gets a 3-letter code plus NUL. */
void smbus_edid_identity(const uint8_t *edid, char vendor[4], uint16_t *product,
                         uint32_t *serial);

/* The panel's native mode -- the first detailed timing descriptor, which is
 * the resolution the display actually wants to be driven at. */
bool smbus_edid_preferred_mode(const uint8_t *edid, uint32_t *w, uint32_t *h);

#endif
