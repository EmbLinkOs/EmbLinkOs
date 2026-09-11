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
 * TWO KINDS OF MEMORY, one witness. `mmap` maps the buffer anonymously;
 * `heap` takes it from malloc, which is sbrk underneath -- the memory every
 * ordinary program lives in. The heap became a pageable mapping after mmap
 * did, and this is the run that says so.
 *
 * A THIRD MODE, `hot`, is the one that tells a fault-order LRU from a real
 * one: a quarter of the pages are re-read every round while a fresh quarter
 * of cold pages streams past. A reclaimer that evicts the OLDEST ARRIVAL
 * evicts the hot set every round -- it arrived first -- and every round pays
 * for it again. One that asks the hardware whether a page was referenced
 * keeps the hot set and pays once. The kernel counts the swap-ins.
 *
 *   run /data/apps/swapper/swapper.elf 256 [mmap|heap|hot]  (MiB; default 64, mmap)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    int heap = (argc > 2 && strcmp(argv[2], "heap") == 0);
    uint64_t *m;
    if (heap) {
        m = malloc(len + PAGE);
        if (!m) { printf("swapper: malloc of %lu MiB failed\n", mib); return 250; }
        m = (uint64_t *)(((uintptr_t)m + PAGE - 1) & ~(uintptr_t)(PAGE - 1));   /* page-align the pattern */
    } else {
        m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { printf("swapper: mmap of %lu MiB failed\n", mib); return 250; }
    }

    for (size_t p = 0; p < npages; p++) {
        uint64_t *w = m + p * WORDS;
        for (size_t i = 0; i < WORDS; i++)
            w[i] = pattern(p, i);
    }

    unsigned long bad = 0;
    int hot = (argc > 2 && strcmp(argv[2], "hot") == 0);
    if (hot) {
        size_t H = npages / 4, S = (npages - H) / 4;
        for (int round = 0; round < 4; round++) {
            for (size_t p = 0; p < H; p++) {                       /* the hot quarter */
                const uint64_t *w = m + p * WORDS;
                if (w[0] != pattern(p, 0) || w[WORDS - 1] != pattern(p, WORDS - 1)) bad++;
            }
            for (size_t p = H + (size_t)round * S; p < H + (size_t)(round + 1) * S && p < npages; p++) {
                const uint64_t *w = m + p * WORDS;                  /* a cold quarter, once */
                if (w[0] != pattern(p, 0) || w[WORDS - 1] != pattern(p, WORDS - 1)) bad++;
            }
        }
    } else
    for (size_t p = npages; p-- > 0; ) {
        const uint64_t *w = m + p * WORDS;
        for (size_t i = 0; i < WORDS; i++)
            if (w[i] != pattern(p, i)) { bad++; break; }
    }

    printf("swapper: %s: %lu MiB (%lu pages) written and read back, %lu pages wrong\n",
           hot ? "hot" : heap ? "heap" : "mmap", mib, (unsigned long)npages, bad);
    if (!heap) munmap(m, len);      /* the heap block is left to exit: that path must return it too */
    return bad > 200 ? 200 : (int)bad;
}
