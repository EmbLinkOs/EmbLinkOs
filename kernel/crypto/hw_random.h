#ifndef EMBLINK_HW_RANDOM_H
#define EMBLINK_HW_RANDOM_H
#include <stdint.h>
/* Cryptographic nonces from x86 RDSEED/RDRAND. Bounded retries, no timer,
 * unseeded PRNG or deterministic fallback. Returns -1 and wipes the entire
 * output if the CPU cannot supply the requested bytes. */
int crypto_hw_random(uint8_t *,unsigned);
#endif
