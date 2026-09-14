/* The chip that remembers what booted. Secure Boot proves the firmware would
 * only run a signed loader; PCRs are the other half -- extend-only registers
 * whose value can be reached only by measuring the same things in the same
 * order, which is what lets a disk key be sealed to a boot state.
 *
 * NOTHING PAST THE PROBE HAS EVER RUN: this host has no swtpm and no real
 * TPM. See the .c. */
#ifndef _EMBK_TPM_H_
#define _EMBK_TPM_H_
#include <stdint.h>
#include "include/types.h"
bool     tpm_init(void);
bool     tpm_present(void);
bool     tpm_is_crb(void);
uint32_t tpm_vendor(void);
uint32_t tpm_extends(void);
int      tpm_pcr_extend(uint32_t pcr, const uint8_t digest[32]);
int      tpm_pcr_read(uint32_t pcr, uint8_t out[32]);
#endif
