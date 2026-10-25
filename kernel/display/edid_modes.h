#ifndef EMBLINK_EDID_MODES_H
#define EMBLINK_EDID_MODES_H
#include "display/mode.h"

/* Every mode the EDID (base block plus `len` - 128 bytes of extensions)
 * offers, de-duplicated, the preferred one flagged. Returns the count, or -1
 * for an EDID that is not a valid one. */
int edid_modes(const uint8_t *edid, unsigned len, struct display_mode *out, unsigned max);

/* One 18-byte detailed timing descriptor. -1 if the slot holds no timing. */
int edid_parse_dtd(const uint8_t *dtd, struct display_mode *m);

/* The standard (DMT/CTA) timing for a size and refresh, or NULL. */
const struct display_mode *edid_known_mode(unsigned width, unsigned height, unsigned hz);

#endif
