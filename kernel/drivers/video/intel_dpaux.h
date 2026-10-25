/* intel_dpaux.h -- the DisplayPort AUX channel on Intel Gen9 display.
 *
 * AUX is the side channel every DP and eDP sink answers on: its capabilities
 * (DPCD), its EDID (I2C carried over AUX), and -- once modesetting -- the
 * link-training handshake. Gen9 has one AUX controller per DDI, A to D. */
#ifndef EMBLINK_INTEL_DPAUX_H
#define EMBLINK_INTEL_DPAUX_H
#include <stdint.h>
#include "drivers/video/intel_regs.h"

#define INTEL_AUX_A 0u
#define INTEL_AUX_B 1u
#define INTEL_AUX_C 2u
#define INTEL_AUX_D 3u
#define INTEL_AUX_CHANNELS 4u

#define DP_AUX_CH_CTL(ch)     (0x64010u + (ch) * 0x100u)
#define DP_AUX_CH_DATA(ch, i) (0x64014u + (ch) * 0x100u + (i) * 4u)

/* One raw AUX message: up to 20 bytes out, the reply into rx. Returns the
 * number of reply bytes, or a negative errno: -EMBK_ETIMEDOUT when no sink
 * answered (which is what an empty port looks like), -EMBK_EIO for a
 * malformed reply, -EMBK_EBUSY if the controller never went idle. */
int intel_aux_transfer(const struct intel_regs *r, unsigned ch,
                       const uint8_t *tx, unsigned txlen,
                       uint8_t *rx, unsigned rxcap);

/* DPCD native reads and writes of any length, split into the 16-byte
 * messages AUX carries, with the sink's DEFER replies retried. 0 or -errno;
 * -EMBK_EIO when the sink NACKs. */
int intel_dpcd_read(const struct intel_regs *r, unsigned ch, uint32_t addr,
                    uint8_t *buf, unsigned len);
int intel_dpcd_write(const struct intel_regs *r, unsigned ch, uint32_t addr,
                     const uint8_t *buf, unsigned len);

/* Read `len` bytes from I2C device `i2c_addr` starting at register `offset`,
 * over AUX -- how an eDP or DP sink's EDID (device 0x50) is read. A complete
 * I2C transaction: start, offset write, reads, stop. 0 or -errno. */
int intel_aux_i2c_read(const struct intel_regs *r, unsigned ch, uint8_t i2c_addr,
                       uint8_t offset, uint8_t *buf, unsigned len);

#endif
