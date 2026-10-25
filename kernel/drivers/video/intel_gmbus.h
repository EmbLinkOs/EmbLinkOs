/* intel_gmbus.h -- GMBUS, the PCH's I2C controller for display DDC.
 *
 * HDMI and DVI have no AUX channel: a monitor's EDID is read over the DDC
 * pins with plain I2C, which on Intel is the GMBUS engine. Pin numbers
 * depend on the PCH generation and come from the VBT's child-device table;
 * these are the defaults the VBT uses. */
#ifndef EMBLINK_INTEL_GMBUS_H
#define EMBLINK_INTEL_GMBUS_H
#include <stdint.h>
#include "drivers/video/intel_regs.h"

/* Cannon/Comet Point (CNP/CMP) DDC pins. Pin 3 is the "misc" pair, which the
 * VBT uses for DDI F, and DDI D is on pin 4 -- i915's cnp_ddc_pin_map calls
 * both "sic", and so does this. */
#define GMBUS_PIN_CNP_DDI_B 1u
#define GMBUS_PIN_CNP_DDI_C 2u
#define GMBUS_PIN_CNP_MISC  3u
#define GMBUS_PIN_CNP_DDI_D 4u
/* Sunrise/Kaby Point (SPT/KBP) DDC pins. */
#define GMBUS_PIN_SPT_DDI_C 4u
#define GMBUS_PIN_SPT_DDI_B 5u
#define GMBUS_PIN_SPT_DDI_D 6u

#define GMBUS_MAX_READ 256u

/* Read `len` (1..256) bytes from I2C device `i2c_addr`, register `offset`, on
 * DDC pin pair `pin`. 0 or -errno: -EMBK_ENXIO when nothing answers on that
 * pin (an empty connector), -EMBK_ETIMEDOUT if the engine stalls. The engine
 * is left disabled afterwards whatever happened. */
int intel_gmbus_read(const struct intel_regs *r, unsigned pin, uint8_t i2c_addr,
                     uint8_t offset, uint8_t *buf, unsigned len);

#endif
