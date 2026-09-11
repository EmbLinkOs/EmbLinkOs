/* swapper.c -- anonymous memory larger than RAM, and the program survives.
 *
 * Maps N MiB anonymous, writes a pattern through every word of every page,
 * then reads every page back and counts the pages that came back wrong. The
 * exit code is that count. `test swap` chooses N from what the machine has
 * free -- more than is free, less than free plus the swap store -- and reads
 * the kernel's counters afterwards to confirm pages actually went out and
 * came back; a run that happened to fit in RAM would pass this program and
 * prove nothing.
 *
 * VERIFIED IN REVERSE, and that is deliberate. After the write pass the pages
 * still in RAM are the most recently written ones; the oldest went to the
 * store. Reading back from the end first hits the resident pages while they
 * are resident, then faults in the swapped ones -- each of which evicts a
 * page that has already been checked. Forward order would evict the pages
 * about to be read, one step ahead of reading them, and turn a check of N
 * pages into N swap-ins. Either order proves the same thing; this one takes
 * a third of the I/O.
 *
 *   run /data/apps/swapper/swapper.elf 256      (MiB; default 64)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>

#define PAGE 4096
#define WORDS (PAGE / sizeof(uint64_t))

/* A pattern unique to every word of every page, so a page served from the
 * wrong slot, or a word from the wrong page, cannot pass by coincidence. */
static inline uint64_t pattern(uint64_t page, uint64_t word) {
    return (page + 1) * 0x9E3779B97F4A7C15ULL ^ (word * 0x0123456789ABCDEFULL) ^ 0xA5A5A5A5A5A5A5A5ULL;
}

int main(int argc, char **argv) {
    unsigned long mib = (argc > 1) ? strtoul(argv[1], NULL, 10) : 64;
    if (mib == 0) mib = 1;
    size_t len = (size_t)mib << 20;
    size_t npages = len / PAGE;

    uint64_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        printf("swapper: mmap of %lu MiB failed\n", mib);
        return 250;
    }

    for (size_t p = 0; p < npages; p++) {
        uint64_t *w = m + p * WORDS;
        for (size_t i = 0; i < WORDS; i++)
            w[i] = pattern(p, i);
    }

    unsigned long bad = 0;
    for (size_t p = npages; p-- > 0; ) {
        const uint64_t *w = m + p * WORDS;
        for (size_t i = 0; i < WORDS; i++)
            if (w[i] != pattern(p, i)) { bad++; break; }
    }

    printf("swapper: %lu MiB (%lu pages) written and read back, %lu pages wrong\n",
           mib, (unsigned long)npages, bad);
    munmap(m, len);
    return bad > 200 ? 200 : (int)bad;
}
