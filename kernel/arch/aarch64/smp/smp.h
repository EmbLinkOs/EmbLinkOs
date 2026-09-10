#ifndef _AARCH64_SMP_H_
#define _AARCH64_SMP_H_

/* Secondary CPU bring-up over PSCI -- docs/ARM64.md phase A9. See smp.c.
 *
 * Safe to call on a machine with one core, or with no PSCI node at all: it
 * says which and returns. */
void smp_bringup(void);

/* PSCI, exposed because it is not only about CPUs: SYSTEM_OFF and
 * SYSTEM_RESET go the same way. `psci_available()` probes the device tree on
 * first use and is safe to call any time after the DTB is parsed; a machine
 * with no PSCI node answers false and `psci_invoke` returns NOT_SUPPORTED. */
#include <stdint.h>
#include "include/types.h"
bool psci_available(void);
int64_t psci_invoke(uint32_t fn, uint64_t a1, uint64_t a2, uint64_t a3);

#endif /* _AARCH64_SMP_H_ */
