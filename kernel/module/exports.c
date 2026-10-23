/* kernel/module/exports.c -- the kernel's contract with loadable modules.
 *
 * EVERY SYMBOL A MODULE MAY CALL, listed once, on purpose. The alternative --
 * letting a module reach any global in the kernel -- is what makes a module
 * system unmaintainable: every module then depends on every internal detail,
 * and renaming a static helper breaks binaries built months ago.
 *
 * So this file is a decision, not a convenience. A symbol appears here because
 * somebody decided a driver written by somebody else may rely on it continuing
 * to exist and continuing to mean the same thing. Adding one is cheap; removing
 * one breaks every module that used it, which is exactly the weight it should
 * carry.
 *
 * A driver compiled INTO the kernel can also export next to its own definition
 * with the same macro -- the linker gathers both. The list here is the core:
 * printing, memory, strings, and the block layer, which is enough for a storage
 * driver to be a module and is deliberately not enough for a module to reach
 * into the scheduler.
 */
#include "module/module.h"
#include "include/kprintf.h"
#include "include/kstring.h"
#include "include/kmalloc.h"
#include "block/block.h"
#include "drivers/bus/pci.h"

/* Printing. The one thing every module does on its first day. */
EMBK_EXPORT(kprintf);

/* Memory. */
EMBK_EXPORT(kmalloc);
EMBK_EXPORT(kfree);

/* Strings and bytes -- also what the compiler itself emits calls to, so a
 * module that never names memcpy still needs it to be resolvable. */
EMBK_EXPORT(memcpy);
EMBK_EXPORT(memset);
EMBK_EXPORT(memcmp);
EMBK_EXPORT(strcmp);
EMBK_EXPORT(strlen);

/* The block layer, so a storage driver can be a module. This is the smallest
 * set that makes a REAL driver possible rather than only a demonstration. */
EMBK_EXPORT(embk_block_register);
EMBK_EXPORT(embk_block_unregister);
EMBK_EXPORT(embk_block_count);
EMBK_EXPORT(embk_block_get);

/* PCI enumeration, so a module can find its own hardware. Reading
 * configuration space is exported; assigning BARs and enabling bus mastering
 * are the machine's business and stay the kernel's. */
EMBK_EXPORT(pci_devices_count);
EMBK_EXPORT(pci_get_device);
EMBK_EXPORT(pci_read_bar);
EMBK_EXPORT(pci_enable_bus_mastering);
