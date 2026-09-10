#ifndef _EMBK_RANDOM_H_
#define _EMBK_RANDOM_H_

#include <stdint.h>
#include <stddef.h>
#include "include/types.h"

/* ==========================================================================
 * THE KERNEL'S ENTROPY, which until now did not exist.
 *
 * Userland had RDRAND on x86 and nothing at all on aarch64 -- getentropy()
 * there returns ENOSYS, honestly, because handing a caller predictable bytes
 * and calling them random is worse than refusing. The kernel itself had no
 * source of unpredictability whatsoever, which is why every process was laid
 * out at the same addresses on every boot: there was nothing to choose them
 * with.
 *
 * This is one CSPRNG for the whole kernel: HMAC_DRBG over SHA-256, as in
 * NIST SP 800-90A, chosen because HMAC-SHA256 is already in the tree and
 * verified, and because HMAC_DRBG's security argument is the simplest of the
 * three constructions -- there is no block cipher mode to get subtly wrong.
 *
 * SEEDED FROM WHATEVER THE MACHINE HAS, in order of quality:
 *   1. a hardware generator -- RDSEED, then RDRAND on x86; RNDR on aarch64
 *      (FEAT_RNG, ARMv8.5) -- when the processor has one;
 *   2. every clock that can be read: the TSC / CNTVCT, the HPET, the RTC,
 *      sampled around events whose timing is not under anyone's control;
 *   3. a jitter loop: the TSC sampled across cache-missing memory traffic,
 *      which contributes some bits on real silicon and fewer under emulation.
 *
 * All of it is mixed in whether or not (1) is present, so a hardware RNG that
 * is backdoored or simply broken cannot make the output WORSE than the clocks
 * alone would. random_quality() says which of the three the seed actually
 * reached, so a caller that must have hardware entropy can ask rather than
 * assume -- and `test random` prints it, because "seeded" from a clock under
 * QEMU is not the same claim as "seeded" from RDSEED on metal.
 *
 * WHAT THIS IS NOT. There is no known-answer test against the NIST CAVP
 * vectors yet (docs/TODO.md); the self-test checks structure -- distinct
 * outputs, reseed changes the stream, a monobit sanity bound -- and says so.
 * Until a KAT lands, "implements HMAC_DRBG" is a claim about the code, not a
 * measurement.
 * ========================================================================== */

enum random_quality {
    RANDOM_UNSEEDED = 0,   /* nothing mixed yet -- only before random_init() */
    RANDOM_JITTER   = 1,   /* clocks + jitter only: no hardware generator     */
    RANDOM_HARDWARE = 2,   /* a hardware generator contributed to the seed    */
};

/* Once, early, before anything that needs an address chosen at random. Safe
 * to call before the scheduler exists: it takes no sleeping lock. */
void random_init(void);

/* Fill `buf` with `len` random bytes. Never fails after random_init(). */
void random_bytes(void *buf, size_t len);

uint64_t random_u64(void);

/* A uniform integer in [0, bound). Unbiased -- rejection sampling, not a
 * modulus -- because a biased address offset is measurably less random than
 * its bit count claims. bound == 0 returns 0. */
uint64_t random_below(uint64_t bound);

/* Mix caller-supplied material into the state -- a device interrupt's
 * timestamp, a user's keystroke timing. Cheap; may be called from IRQ
 * context. Never makes the state worse. */
void random_add_entropy(const void *data, size_t len);

enum random_quality random_quality(void);

/* How much has been drawn, for the self-test and for `power`-style reporting. */
void random_stats(uint64_t *bytes_out, uint64_t *reseeds,
                  uint64_t *hw_seed_words);

/* --- what each architecture provides ------------------------------------ */

/* One word from a hardware generator. false if the processor has none, or if
 * it declined this time (RDSEED/RDRAND may legitimately fail transiently). */
bool arch_hw_random_u64(uint64_t *out);

/* Every clock the architecture can read, pushed into `sink`. Called several
 * times during seeding, so it should be cheap and should read things that
 * MOVE between calls. */
void arch_random_seed_extra(void (*sink)(const void *data, size_t len));

#endif /* _EMBK_RANDOM_H_ */
