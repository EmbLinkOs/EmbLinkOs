#ifndef _EMBK_SWAPTEST_H_
#define _EMBK_SWAPTEST_H_
#include "include/types.h"

/* THE SWAP WITNESS, DRIVEN. Runs user/bin/swapper.elf at `witness` against
 * the machine's actual free memory -- more than is free, less than free plus
 * half the store -- and judges each run on the kernel's own counters: pages
 * went out and came back, every slot has an owner, the anonymous pages in
 * the system are conserved. `quick` runs the mmap and heap witnesses only;
 * the full run adds the hot-set A/B (fault-order vs. second chance).
 *
 * Shared between the x86 console (`test swap`) and the aarch64 boot test,
 * which has no console: the same claims, measured on both machines. Prints
 * its evidence as it goes; returns 0 when every claim held. */
int swap_selftest_run(const char *witness, bool quick);

#endif
