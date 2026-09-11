/* user/bin/pieprobe.c -- the position-independent-executable witness.
 *
 * Built ET_DYN (-pie, newlib-pie.ld, the -fPIC newlib) and loaded by
 * kernel/loader/elf.c at a bias the process drew from the kernel's CSPRNG. It
 * reports where it actually landed, and the kernel's `test pie` spawns it twice
 * and proves the two answers differ.
 *
 * WHAT IT PROVES, and why each part is here rather than "it started, ship it".
 * A PIE that merely reaches main() proves almost nothing: the code the compiler
 * could make PC-relative works by construction. Everything that can go wrong
 * with a load bias goes wrong in the four things below, and all four are
 * checked before the address is reported.
 *
 *   1. An initialised pointer to a global. That is an R_X86_64_RELATIVE (or
 *      R_AARCH64_RELATIVE) entry in .rela.dyn, written into the image at link
 *      time as if the load address were zero. If the kernel did not apply it,
 *      this pointer is a small integer and dereferencing it faults.
 *
 *   2. A pointer to a string literal, same relocation, pointing the other way
 *      across the W^X boundary (writable .data -> read-only .rodata).
 *
 *   3. A static constructor. .init_array holds function POINTERS, so an
 *      unrelocated array sends crt0's loop through a table of small integers --
 *      the most spectacular way this can fail and the easiest to miss, because
 *      a program with no constructors will not notice.
 *
 *   4. A __thread variable. TLS geometry is the one thing a load bias corrupts
 *      QUIETLY: the sizes come from the linker, and a size that has been
 *      biased is enormous rather than wrong-looking. See newlib-body.ld.
 *
 * The exit code is the page index of this program's own text INSIDE the
 * kernel's executable window, which is what makes the check automatic: the
 * window starts at a page number that is exactly 2^32, so masking off the low
 * 31 bits of (address >> 12) leaves the offset and nothing else.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int         g_target = 0xA5;
static int        *g_ptr    = &g_target;          /* (1) RELATIVE into .data   */
static const char *g_text   = "pieprobe";        /* (2) RELATIVE into .rodata */
static int         g_ctor_ran = 0;                /* (3) set by the ctor below */
static __thread int g_tls   = 0x5A;               /* (4) TLS template          */

__attribute__((constructor))
static void ran_before_main(void) { g_ctor_ran = 1; }

/* The address to report. Its own function so the compiler cannot fold it into
 * something with no address of its own, and so the symbol is easy to find in a
 * disassembly when this test fails. */
__attribute__((noinline)) static uintptr_t text_address(void)
{
    return (uintptr_t)(void *)&text_address;
}

int main(void)
{
    uintptr_t text = text_address();

    int ok = 1;
    if (g_ptr != &g_target || *g_ptr != 0xA5) {
        printf("pieprobe: FAIL relocated data pointer (%p -> %p)\n",
               (void *)g_ptr, (void *)&g_target);
        ok = 0;
    }
    if (!g_text || strcmp(g_text, "pieprobe") != 0) {
        printf("pieprobe: FAIL relocated rodata pointer\n");
        ok = 0;
    }
    if (!g_ctor_ran) {
        printf("pieprobe: FAIL constructor did not run\n");
        ok = 0;
    }
    if (g_tls != 0x5A) {
        printf("pieprobe: FAIL __thread initial value is %d\n", g_tls);
        ok = 0;
    }
    g_tls = 0x11;
    if (g_tls != 0x11) {
        printf("pieprobe: FAIL __thread is not writable\n");
        ok = 0;
    }

    /* (5) The PIC libc is a SECOND newlib, built by the same script with the
     * same options plus -fPIC. "Same options" is a claim about a build that
     * happened somewhere else, and the option that matters most here is the one
     * a stock newlib omits: C99 printf formats. Without it %zu prints the
     * literal "zu" and %llu is dropped -- silently, in every PIE, with nothing
     * to see until a number comes out wrong. One line, checked here, because
     * this program is the only thing in the tree that links that libc. */
    {
        char b[64];
        size_t z = 1234;
        unsigned long long q = 5000000000ULL;
        snprintf(b, sizeof b, "%zu/%llu", z, q);
        if (strcmp(b, "1234/5000000000") != 0) {
            printf("pieprobe: FAIL the PIC libc has no C99 formats: \"%s\"\n", b);
            ok = 0;
        }
    }

    printf("pieprobe: text %p  data %p  rodata %p  %s\n",
           (void *)text, (void *)&g_target, (void *)g_text, ok ? "checks ok" : "CHECKS FAILED");
    fflush(stdout);

    /* 0x7FFFFFFF cannot be a page index -- the window is 64 GiB, so the index
     * is under 2^24 -- which keeps "ran but lied" distinguishable from every
     * legitimate answer, including an offset that happens to be zero. */
    if (!ok) return 0x7FFFFFFF;
    return (int)((text >> 12) & 0x7FFFFFFF);
}
