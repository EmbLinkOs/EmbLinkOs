/* Three small devices the machine offers and this kernel was ignoring:
 * pvpanic, the i6300ESB watchdog, and QEMU's debug-exit port. See the .c. */
#ifndef _EMBK_PLATFORM_MISC_H_
#define _EMBK_PLATFORM_MISC_H_

#include <stdint.h>
#include "include/types.h"

struct pci_device;

/* Probe for all three. After pci_init. */
void platform_misc_init(void);

bool platform_pvpanic_present(void);
bool platform_watchdog_present(void);

/* Tell the host this guest has panicked. Silent no-op without the device. */
void platform_pvpanic_notify(void);

/* Arm the watchdog for ~`seconds` (0 disarms). Refuses past the device's
 * 31-second stage limit rather than silently rounding down to a reset that
 * comes sooner than the caller believes. */
bool platform_watchdog_arm(const struct pci_device *pci, uint32_t seconds);
void platform_watchdog_pet(void);

/* END THE EMULATOR with a status code, so a test can RETURN a verdict rather
 * than print a sentence for a harness to grep. QEMU's exit status is
 * (code << 1) | 1, so it is never 0 -- these two are the agreed values.
 * A no-op on real hardware, which is why it is safe to leave in place. */
#define PLATFORM_EXIT_PASS 0x10   /* -> QEMU exit status 33 */
#define PLATFORM_EXIT_FAIL 0x11   /* -> QEMU exit status 35 */
void platform_debug_exit(uint8_t code);

#endif
