/* The filesystem on a CD: read-only, because the medium is. A file is a start
 * block and a length; there is nothing else to it. See the .c. */
#ifndef _EMBK_ISO9660_H_
#define _EMBK_ISO9660_H_
#include <stdint.h>
#include "include/types.h"
struct embk_block_device;
bool        iso9660_probe(struct embk_block_device *dev);
int         iso9660_mount(struct embk_block_device *dev, const char *at);
const void *iso9660_vfs_ops_ptr(void);
#endif
