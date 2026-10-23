#ifndef _EMBK_PIETEST_H_
#define _EMBK_PIETEST_H_

/* THE POSITION-INDEPENDENT-EXECUTABLE WITNESS, DRIVEN.
 *
 * Spawns the ET_DYN program at `witness` twice and judges the two runs on
 * where they actually landed. The program (user/tests/pieprobe/pieprobe.c) verifies its
 * own relocated pointers, its constructor and its __thread variable, then exits
 * with the page index of its own text inside the kernel's executable window --
 * so one integer says "I am position-independent, I was relocated correctly,
 * and here is where". Two runs must answer differently.
 *
 * Shared between the x86 console (`test pie`) and the aarch64 boot test, which
 * has no console: the same claim, measured on both machines, because the
 * relocation types and the linker are the only parts that differ.
 *
 * Prints its evidence as it goes. Returns 0 when every claim held, and a
 * negative errno when the witness is not on the image -- which is not a
 * failure of the kernel, only of the build (no -fPIC libc, no PIE), and the
 * callers report it as such. */
int pie_selftest_run(const char *witness);

#endif
