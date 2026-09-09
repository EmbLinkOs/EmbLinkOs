#ifndef _AARCH64_SMP_H_
#define _AARCH64_SMP_H_

/* Secondary CPU bring-up over PSCI -- docs/ARM64.md phase A9. See smp.c.
 *
 * Safe to call on a machine with one core, or with no PSCI node at all: it
 * says which and returns. */
void smp_bringup(void);

#endif /* _AARCH64_SMP_H_ */
