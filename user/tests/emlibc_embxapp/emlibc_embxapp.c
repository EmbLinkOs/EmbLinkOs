/* emlibc_embxapp.c -- the convergence artifact. Compiled against emlibc,
 * linked by EmbLD into a native EMBX binary that DECLARES {FILESYSTEM} in its
 * capability table (embld --embx --cap filesystem). The kernel's EMBX loader
 * grants the process exactly its declared set -- so the capability comes from
 * the BINARY, not from what the spawner chose to hand it.
 *
 * It reports the set it was BORN with and exits 42 iff that set is exactly
 * {FILESYSTEM} -- proving the declaration flowed binary -> loader -> process.
 * The whole owned stack in one file: emlibc (libc) + EmbLD (EMBX emitter) +
 * EMBX (format) + the kernel loader.
 */
#include <stdio.h>
#include <process.h>

int main(void)
{
    unsigned long c = em_getcaps();
    int fs  = em_have_cap(EM_CAP_FILESYSTEM);
    int net = em_have_cap(EM_CAP_NETWORK);

    FILE *f = fopen("/data/tmp/embxapp.out", "w");
    if (f) {
        fprintf(f, "embx: born caps=0x%lx fs=%d net=%d\n", c, fs, net);
        fclose(f);
    }

    /* Born with exactly what the EMBX declared: FILESYSTEM, and nothing the
     * grantor also held (e.g. NETWORK) that the binary did not ask for. */
    return (fs && !net) ? 42 : 1;
}
