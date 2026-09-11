/* aslrprobe.c -- print where this process was put, so a person can run it
 * twice and watch the addresses move.
 *
 * `test aslr` is the assertion; this is the demonstration. Five addresses,
 * four of which the kernel chooses per process and one it does not (yet):
 *
 *   heap    embk_sbrk(0)          the sbrk window
 *   mmap    a fresh mapping       the mmap window
 *   stack   the address of a local
 *   dylib   a libembk.so symbol   the dynamic library's load base
 *   text    a function in THIS binary -- fixed at 0x400000, because the app
 *           is ET_EXEC; moving it is the PIE work in docs/TODO.md. Printed so
 *           the thing that does NOT move is visible next to the things that do.
 *
 *   run /data/apps/aslrprobe/aslrprobe.elf     (twice)
 */
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include "embk.h"
#include "em.h"      /* em_tokens_ lives in libembk.so */

static void here(void) { }

int main(void) {
    long heap = embk_sbrk(0);
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    volatile int local = 0;

    printf("aslrprobe: heap  %p\n", (void *)(uintptr_t)heap);
    printf("aslrprobe: mmap  %p\n", m);
    printf("aslrprobe: stack %p\n", (void *)&local);
    /* A DATA object inside libembk.so, not a function: taking a function's
     * address in a dynamically-linked app yields the PLT stub, which lives in
     * the APP's text and moves with nothing. The first version printed
     * 0x4033f0 here and called it the library. em_tokens_() hands back a
     * pointer to a static inside the .so, which is where the library actually
     * is. */
    printf("aslrprobe: dylib %p   (libembk.so: its theme tokens)\n", (const void *)em_tokens_());
    printf("aslrprobe: text  %p   (this binary -- fixed until PIE)\n", (void *)here);
    if (m != MAP_FAILED) munmap(m, 4096);
    return 0;
}
