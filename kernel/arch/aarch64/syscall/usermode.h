#ifndef _AARCH64_USERMODE_H
#define _AARCH64_USERMODE_H

#include <stdint.h>

/* Run the EL0 probe -- docs/ARM64.md phase A5.
 *
 * Builds a user address space in TTBR0, copies the probe program into it,
 * drops to EL0, and returns when the program calls exit(). The return value is
 * the exit code the program passed, or negative if it could not be started.
 *
 * TEMPORARY, like sched/bringup.c: A6 replaces this with the real ELF loader
 * and process creation path that x86 already uses. Its job is to prove the
 * transition itself -- eret out, svc back in, eret out again -- before
 * anything more complicated depends on it. */
int64_t el0_probe_run(void);

#endif /* _AARCH64_USERMODE_H */
