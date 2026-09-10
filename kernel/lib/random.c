#include "lib/random.h"
#include "crypto/hmac.h"
#include "crypto/sha256.h"
#include "include/spinlock.h"
#include "include/kstring.h"
#include "include/kprintf.h"
#include "drivers/timer/timer.h"

/* ==========================================================================
 * HMAC_DRBG (SP 800-90A §10.1.2), SHA-256.
 *
 * State is two 32-byte values, K and V. Every operation is built from one
 * primitive, HMAC_DRBG_Update, which folds optional input into K and V:
 *
 *   K = HMAC(K, V || 0x00 || data);  V = HMAC(K, V)
 *   if data:  K = HMAC(K, V || 0x01 || data);  V = HMAC(K, V)
 *
 * Generate is then V = HMAC(K, V) repeated, emitting each V, followed by an
 * Update with no data so the emitted values cannot be run backwards to the
 * state (backtracking resistance). Reseed is an Update with the new entropy.
 * That is the entire construction; there is nothing else to get wrong.
 * ========================================================================== */

#define DRBG_OUTLEN   SHA256_DIGEST_SIZE
#define DRBG_RESEED_INTERVAL  (1ull << 20)    /* generates between reseeds */
#define DRBG_MAX_REQUEST      (1u << 16)      /* bytes per generate call    */

static struct {
    uint8_t  K[DRBG_OUTLEN];
    uint8_t  V[DRBG_OUTLEN];
    uint64_t reseed_counter;
    bool     instantiated;
    enum random_quality quality;

    uint64_t bytes_out;
    uint64_t reseeds;
    uint64_t hw_words;       /* hardware-generator words that went into seeds */
} g_drbg;

static spinlock_t g_drbg_lock = SPINLOCK_INIT;

/* --- the one primitive ----------------------------------------------------- */
static void drbg_update(const uint8_t *data, size_t len) {
    /* HMAC(K, V || 0x00 || data): built in a small stack buffer. Seed material
     * is bounded (see drbg_seed) so this never needs a heap allocation, which
     * matters because random_init() runs before there is a heap to allocate
     * from. */
    uint8_t msg[DRBG_OUTLEN + 1 + 256];
    if (len > 256) len = 256;

    memcpy(msg, g_drbg.V, DRBG_OUTLEN);
    msg[DRBG_OUTLEN] = 0x00;
    if (len) memcpy(msg + DRBG_OUTLEN + 1, data, len);
    hmac_sha256(g_drbg.K, DRBG_OUTLEN, msg, DRBG_OUTLEN + 1 + len, g_drbg.K);
    hmac_sha256(g_drbg.K, DRBG_OUTLEN, g_drbg.V, DRBG_OUTLEN, g_drbg.V);

    if (len == 0) return;

    memcpy(msg, g_drbg.V, DRBG_OUTLEN);
    msg[DRBG_OUTLEN] = 0x01;
    memcpy(msg + DRBG_OUTLEN + 1, data, len);
    hmac_sha256(g_drbg.K, DRBG_OUTLEN, msg, DRBG_OUTLEN + 1 + len, g_drbg.K);
    hmac_sha256(g_drbg.K, DRBG_OUTLEN, g_drbg.V, DRBG_OUTLEN, g_drbg.V);
}

/* --- seed gathering -------------------------------------------------------- */

/* Seed material accumulates through SHA-256 rather than being concatenated:
 * the sources are of wildly different quality and length, and hashing them
 * together means no single source can dominate the layout of the seed. The
 * digest is what feeds drbg_update(). */
static struct sha256_ctx g_pool;
static size_t            g_pool_bytes;

static void pool_sink(const void *data, size_t len) {
    sha256_update(&g_pool, data, len);
    g_pool_bytes += len;
}

/* A jitter loop. Not a strong source and not claimed as one: it samples the
 * cycle counter across memory traffic whose exact timing depends on cache
 * state, interrupts and the memory system, all of which are outside this
 * code's control. On real silicon that is worth some bits per sample; under
 * an emulator it is worth fewer, and random_quality() says which world the
 * seed came from. */
static void jitter_sink(void (*sink)(const void *, size_t)) {
    static volatile uint64_t scratch[512];
    uint64_t prev = time_get_ns();
    for (int i = 0; i < 256; i++) {
        /* Touch a stride that defeats the prefetcher, then time it. */
        scratch[(i * 37) & 511] += prev;
        uint64_t now = time_get_ns();
        uint64_t delta = now - prev;
        prev = now;
        sink(&delta, sizeof delta);
    }
}

/* Gather one seed's worth of material into the pool and fold it in. Returns
 * the quality actually reached. Lock held by the caller. */
static enum random_quality drbg_seed_locked(void) {
    sha256_init(&g_pool);
    g_pool_bytes = 0;
    enum random_quality q = RANDOM_JITTER;

    /* 1. Hardware, if any. 32 words -- 256 bytes -- is plenty for a 256-bit
     *    security level with margin, and it is mixed rather than used raw so
     *    a bad generator cannot lower the floor the clocks set. */
    int got = 0;
    for (int i = 0; i < 32; i++) {
        uint64_t w;
        if (arch_hw_random_u64(&w)) { pool_sink(&w, sizeof w); got++; }
    }
    if (got) { q = RANDOM_HARDWARE; g_drbg.hw_words += (uint64_t)got; }

    /* 2. Every clock, several times, with the jitter loop between so the
     *    readings differ by something other than a constant. */
    for (int round = 0; round < 4; round++) {
        arch_random_seed_extra(pool_sink);
        jitter_sink(pool_sink);
    }

    /* 3. Whatever state the DRBG already had, so a reseed never discards the
     *    entropy of the previous seed -- it can only add to it. */
    pool_sink(g_drbg.K, DRBG_OUTLEN);
    pool_sink(g_drbg.V, DRBG_OUTLEN);

    uint8_t seed[SHA256_DIGEST_SIZE];
    sha256_final(&g_pool, seed);
    drbg_update(seed, sizeof seed);
    memset(seed, 0, sizeof seed);

    g_drbg.reseed_counter = 1;
    g_drbg.reseeds++;
    return q;
}

/* --- public API --------------------------------------------------------------- */

void random_init(void) {
    spin_lock(&g_drbg_lock);
    if (!g_drbg.instantiated) {
        /* Instantiate per §10.1.2.3: K = 0x00.., V = 0x01.., then Update with
         * the seed. Done by drbg_seed_locked() folding the seed in. */
        memset(g_drbg.K, 0x00, DRBG_OUTLEN);
        memset(g_drbg.V, 0x01, DRBG_OUTLEN);
        g_drbg.instantiated = true;
    }
    g_drbg.quality = drbg_seed_locked();
    spin_unlock(&g_drbg_lock);

    kprintf("random: seeded from %s (%llu hardware words, %llu bytes of "
            "clock and jitter material)\n",
            g_drbg.quality == RANDOM_HARDWARE ? "a hardware generator + clocks"
                                              : "clocks and jitter ONLY",
            (unsigned long long)g_drbg.hw_words,
            (unsigned long long)g_pool_bytes);
}

static void drbg_generate_locked(uint8_t *out, size_t len) {
    if (g_drbg.reseed_counter >= DRBG_RESEED_INTERVAL)
        g_drbg.quality = drbg_seed_locked();

    while (len) {
        hmac_sha256(g_drbg.K, DRBG_OUTLEN, g_drbg.V, DRBG_OUTLEN, g_drbg.V);
        size_t n = len < DRBG_OUTLEN ? len : DRBG_OUTLEN;
        memcpy(out, g_drbg.V, n);
        out += n; len -= n;
    }
    drbg_update(0, 0);              /* backtracking resistance */
    g_drbg.reseed_counter++;
}

void random_bytes(void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        size_t n = len < DRBG_MAX_REQUEST ? len : DRBG_MAX_REQUEST;
        spin_lock(&g_drbg_lock);
        if (!g_drbg.instantiated) {
            /* Called before random_init(): seed now rather than hand out the
             * all-zero instantiation constants as if they were random. */
            memset(g_drbg.K, 0x00, DRBG_OUTLEN);
            memset(g_drbg.V, 0x01, DRBG_OUTLEN);
            g_drbg.instantiated = true;
            g_drbg.quality = drbg_seed_locked();
        }
        drbg_generate_locked(p, n);
        g_drbg.bytes_out += n;
        spin_unlock(&g_drbg_lock);
        p += n; len -= n;
    }
}

uint64_t random_u64(void) {
    uint64_t v;
    random_bytes(&v, sizeof v);
    return v;
}

uint64_t random_below(uint64_t bound) {
    if (bound == 0) return 0;
    /* Rejection sampling: draw again while the draw falls in the partial
     * top bucket that a modulus would fold unevenly. For any bound the
     * expected number of draws is under 2. */
    uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
    uint64_t v;
    do { v = random_u64(); } while (v >= limit);
    return v % bound;
}

void random_add_entropy(const void *data, size_t len) {
    if (!data || !len) return;
    /* Folded straight into the state, not into the pool: the pool is only
     * live during a seed. Update never reduces entropy, so this is safe to do
     * with material of any quality, including none. */
    spin_lock(&g_drbg_lock);
    if (g_drbg.instantiated)
        drbg_update(data, len);
    spin_unlock(&g_drbg_lock);
}

enum random_quality random_quality(void) { return g_drbg.quality; }

void random_stats(uint64_t *bytes_out, uint64_t *reseeds, uint64_t *hw_seed_words) {
    if (bytes_out)     *bytes_out     = g_drbg.bytes_out;
    if (reseeds)       *reseeds       = g_drbg.reseeds;
    if (hw_seed_words) *hw_seed_words = g_drbg.hw_words;
}
