#ifndef _AARCH64_ITS_H_
#define _AARCH64_ITS_H_

#include <stdint.h>
#include "include/types.h"

/* The GIC Interrupt Translation Service: MSI on aarch64. See its.c.
 *
 * Absence is not a failure -- a machine without an ITS still delivers legacy
 * INTx, which is what every driver here falls back to. */
bool its_init(void);
bool its_present(void);

/* Allocate an LPI for `devid` (the PCI RID) and return the message that
 * delivers it: the address to write and the value to write there. */
bool its_map_msi(uint32_t devid, uint32_t *out_intid, uint64_t *out_addr,
                 uint32_t *out_data);

/* Ask the ITS to translate and deliver one interrupt to itself, proving the
 * whole path rather than just that initialisation returned. See its.c. */
bool its_selftest(void);

#endif /* _AARCH64_ITS_H_ */
